#if __has_include(<ESPAsyncWebServer.h>)

#include "HttpMCPServer.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cstdlib>
#include <cstring>

namespace {

/* Accumulated POST body, stored in request->_tempObject. ESPAsyncWebServer
 * releases _tempObject with free() when a request dies (including aborted
 * uploads), so this must be one malloc() block with no destructor — anything
 * else either leaks or corrupts the heap on disconnect. */
struct BodyBuffer {
    size_t received;
    uint8_t status;
    char data[1];  // over-allocated to hold the body plus a NUL terminator
};

enum : uint8_t {
    BODY_OK = 0,
    BODY_TOO_LARGE = 1,
    BODY_NO_LENGTH = 2,
};

}  // namespace

HttpMCPServer::HttpMCPServer(uint16_t port, const String& name, const String& version, const String& instructions)
    : MCPServerBase(name, version, instructions), port(port) {
    server = new AsyncWebServer(port);
    setupWebServer();
    setupMDNS();
}

HttpMCPServer::~HttpMCPServer() {
    if (server) {
        delete server;
        server = nullptr;
    }
}

void HttpMCPServer::setupWebServer() {
    /* The body callback only accumulates; the response is always sent from the
     * onRequest callback, which runs once the request is complete — including
     * when there is no body at all (previously such POSTs hung with no reply). */
    server->on(
        "/mcp", HTTP_POST, [this](AsyncWebServerRequest* request) { handlePostComplete(request); }, NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            BodyBuffer* body = static_cast<BodyBuffer*>(request->_tempObject);
            if (!body) {
                if (index != 0) {
                    return;  // first-chunk allocation failed; drop the rest
                }
                uint8_t status = BODY_OK;
                size_t cap = total;
                if (total == 0) {
                    // Chunked upload with no Content-Length; we can't size the buffer.
                    status = BODY_NO_LENGTH;
                    cap = 0;
                } else if (total > MCP_HTTP_MAX_BODY_SIZE) {
                    status = BODY_TOO_LARGE;
                    cap = 0;
                }
                body = static_cast<BodyBuffer*>(malloc(sizeof(BodyBuffer) + cap));
                if (!body) {
                    return;  // handlePostComplete reports the OOM
                }
                body->received = 0;
                body->status = status;
                request->_tempObject = body;
            }

            if (body->status != BODY_OK || index >= total) {
                return;
            }
            if (index + len > total) {
                len = total - index;  // clamp clients that overshoot their declared Content-Length
            }
            memcpy(body->data + index, data, len);
            body->received = index + len;
        });

    server->on("/mcp", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response =
            request->beginResponse(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
        response->addHeader("Allow", "POST, GET");
        request->send(response);
    });

    server->on("/mcp", HTTP_GET, [this](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response =
            request->beginResponse(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
        response->addHeader("Allow", "POST");
        request->send(response);
    });

    server->onNotFound([this](AsyncWebServerRequest* request) {
        sendJSONRPCError(request, 404, ErrorCode::INVALID_REQUEST, "Path Not Found");
    });

    server->begin();
}

std::string HttpMCPServer::generateUUID() {
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[16];
    for (uint8_t i = 0; i < sizeof(bytes); i += 4) {
        uint32_t value = esp_random();
        bytes[i] = (value >> 24) & 0xFF;
        bytes[i + 1] = (value >> 16) & 0xFF;
        bytes[i + 2] = (value >> 8) & 0xFF;
        bytes[i + 3] = value & 0xFF;
    }
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char buf[37];
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            buf[pos++] = '-';
        }
        buf[pos++] = hex[(bytes[i] >> 4) & 0x0F];
        buf[pos++] = hex[bytes[i] & 0x0F];
    }
    buf[pos] = '\0';
    return std::string(buf, pos);
}

void HttpMCPServer::setupMDNS() {
    if (!MDNS.begin(serverName.c_str())) {
        return;
    }

    std::string usn = "uuid:" + generateUUID() + "::mcp:device";
    std::string endpoint =
        "http://" + std::string(WiFi.localIP().toString().c_str()) + ":" + std::to_string(port) + "/mcp";

    MDNS.addService("_mcp", "_tcp", port);
    MDNS.addServiceTxt("_mcp", "_tcp", "path", "/mcp");
    MDNS.addServiceTxt("_mcp", "_tcp", "endpoint", endpoint.c_str());
    MDNS.addServiceTxt("_mcp", "_tcp", "device_type", "mcp:device");
    MDNS.addServiceTxt("_mcp", "_tcp", "usn", usn.c_str());
}

void HttpMCPServer::sendJSONRPCError(AsyncWebServerRequest* request, int httpCode, ErrorCode rpcCode,
                                     const char* message) {
    JsonDocument nullId;
    nullId.set(nullptr);
    MCPResponse error = createJSONRPCError(httpCode, static_cast<int>(rpcCode), nullId.as<JsonVariantConst>(), message);
    sendMCPResponse(request, error);
}

void HttpMCPServer::handlePostComplete(AsyncWebServerRequest* request) {
    BodyBuffer* body = static_cast<BodyBuffer*>(request->_tempObject);

    if (!body) {
        if (request->contentLength() == 0) {
            // No body at all; let the parser produce the JSON-RPC parse error.
            handleJsonBody(request, "");
            return;
        }
        const String& contentType = request->contentType();
        if (contentType.length() > 0 && !contentType.startsWith("application/json")) {
            /* ESPAsyncWebServer consumes urlencoded/multipart bodies itself and
             * never invokes our body callback — not an allocation failure. */
            sendJSONRPCError(request, 415, ErrorCode::INVALID_REQUEST, "Content-Type must be application/json");
        } else {
            // The body callback could not allocate the accumulation buffer.
            sendJSONRPCError(request, 500, ErrorCode::INTERNAL_ERROR, "Out of memory");
        }
        return;
    }

    const uint8_t status = body->status;
    const size_t received = body->received;
    if (status == BODY_OK && received == request->contentLength()) {
        body->data[received] = '\0';
        handleJsonBody(request, body->data);
    } else if (status == BODY_TOO_LARGE) {
        sendJSONRPCError(request, 413, ErrorCode::INVALID_REQUEST, "Request body too large");
    } else if (status == BODY_NO_LENGTH) {
        sendJSONRPCError(request, 411, ErrorCode::INVALID_REQUEST, "Content-Length required");
    } else {
        sendJSONRPCError(request, 400, ErrorCode::INVALID_REQUEST, "Incomplete request body");
    }

    free(body);
    request->_tempObject = nullptr;
}

void HttpMCPServer::handleJsonBody(AsyncWebServerRequest* request, const char* body) {
    if (!validateOriginHeader(request)) {
        sendJSONRPCError(request, 403, ErrorCode::INVALID_REQUEST, "Forbidden Origin");
        return;
    }

    MCPRequest mcpReq = parseRequest(body);

    if (!validateProtocolVersionHeader(request, mcpReq)) {
        MCPResponse invalidVersion = createJSONRPCError(400, static_cast<int>(ErrorCode::INVALID_PARAMS), mcpReq.id(),
                                                        "Unsupported MCP-Protocol-Version");
        sendMCPResponse(request, invalidVersion);
        return;
    }

    MCPResponse mcpRes = handle(mcpReq);
    sendMCPResponse(request, mcpRes);
}

void HttpMCPServer::sendMCPResponse(AsyncWebServerRequest* request, const MCPResponse& response) {
    std::string jsonResponse = serializeResponse(response);
    if (!response.hasBody() || jsonResponse.empty()) {
        request->send(response.code);
        return;
    }

    AsyncWebServerResponse* httpResponse =
        request->beginResponse(response.code, "application/json", jsonResponse.c_str());
    httpResponse->addHeader("MCP-Protocol-Version", PROTOCOL_VERSION);
    request->send(httpResponse);
}

bool HttpMCPServer::validateProtocolVersionHeader(AsyncWebServerRequest* request, const MCPRequest& mcpRequest) {
    (void)mcpRequest;
    if (!request->hasHeader("MCP-Protocol-Version")) {
        return true;
    }

    String value = request->getHeader("MCP-Protocol-Version")->value();
    return isSupportedProtocolVersion(value.c_str());
}

bool HttpMCPServer::validateOriginHeader(AsyncWebServerRequest* request) {
    if (!request->hasHeader("Origin")) {
        return true;
    }
    if (!request->hasHeader("Host")) {
        return false;
    }

    const String& origin = request->getHeader("Origin")->value();
    const String& host = request->getHeader("Host")->value();

    if (origin.startsWith("http://") && origin.length() == 7 + host.length()) {
        return memcmp(origin.c_str() + 7, host.c_str(), host.length()) == 0;
    }
    if (origin.startsWith("https://") && origin.length() == 8 + host.length()) {
        return memcmp(origin.c_str() + 8, host.c_str(), host.length()) == 0;
    }
    return false;
}

#endif  // __has_include(<ESPAsyncWebServer.h>)
