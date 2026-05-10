#if __has_include(<ESPAsyncWebServer.h>)

#include "HttpMCPServer.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_system.h>
#include <new>

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
    server->on(
        "/mcp", HTTP_POST, [this](AsyncWebServerRequest* request) {}, NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            String* body = nullptr;
            if (index == 0) {
                body = new (std::nothrow) String();
                if (!body) {
                    request->send(500, "application/json", "{\"error\":\"Out of memory\"}");
                    return;
                }
                body->reserve(total);
                request->_tempObject = body;
            } else {
                body = static_cast<String*>(request->_tempObject);
            }

            if (!body) {
                request->send(500, "application/json", "{\"error\":\"Request body state missing\"}");
                return;
            }

            body->concat((const char*)data, len);

            if (index + len < total) {
                return;
            }

            String completeBody = *body;
            delete body;
            request->_tempObject = nullptr;
            handleJsonBody(request, completeBody);
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
        JsonDocument nullId;
        nullId.set(nullptr);
        MCPResponse res = createJSONRPCError(404, static_cast<int>(ErrorCode::INVALID_REQUEST),
                                             nullId.as<JsonVariantConst>(), "Path Not Found");
        sendMCPResponse(request, res);
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

void HttpMCPServer::handleJsonBody(AsyncWebServerRequest* request, const String& body) {
    if (!validateOriginHeader(request)) {
        JsonDocument nullId;
        nullId.set(nullptr);
        MCPResponse forbidden = createJSONRPCError(403, static_cast<int>(ErrorCode::INVALID_REQUEST),
                                                   nullId.as<JsonVariantConst>(), "Forbidden Origin");
        sendMCPResponse(request, forbidden);
        return;
    }

    MCPRequest mcpReq = parseRequest(body.c_str());

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
