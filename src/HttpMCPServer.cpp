#if __has_include(<ESPAsyncWebServer.h>)

#include "HttpMCPServer.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <new>

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

bool isJsonContentType(const String& value) {
    static const char expected[] = "application/json";
    const char* cursor = value.c_str();
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }
    for (size_t i = 0; expected[i] != '\0'; ++i, ++cursor) {
        if (std::tolower(static_cast<unsigned char>(*cursor)) != expected[i]) {
            return false;
        }
    }
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }
    return *cursor == '\0' || *cursor == ';';
}

/* One deferred tools/call. Shared between the async_tcp task (the chunked
 * response filler) and the worker task. Reference-counted intrusively and
 * allocated with new (std::nothrow) so that running out of memory on the
 * request path is a graceful HTTP 500 in BOTH exception modes — make_shared
 * would abort the device under -fno-exceptions. One reference belongs to the
 * queue/worker hand-off, one to the response filler; whichever drops last
 * frees the job. */
struct HttpToolJob {
    MCPRequest request;
    std::string response;  // serialized JSON-RPC; valid once done is set
    std::atomic<bool> done{false};
    std::atomic<int> refs{1};

    static void release(HttpToolJob* job) {
        if (job && job->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete job;
        }
    }
};

/* Copyable job handle for the filler std::function (which requires copyable
 * captures): every copy owns one reference. */
class JobRef {
   public:
    explicit JobRef(HttpToolJob* job) : job_(job) {}  // adopts an existing reference
    JobRef(const JobRef& other) : job_(other.job_) {
        if (job_) {
            job_->refs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    JobRef(JobRef&& other) noexcept : job_(other.job_) { other.job_ = nullptr; }
    JobRef& operator=(const JobRef&) = delete;
    ~JobRef() { HttpToolJob::release(job_); }
    HttpToolJob* operator->() const { return job_; }

   private:
    HttpToolJob* job_;
};

}  // namespace

#ifdef MCP_HTTP_TEST_HOOKS
static int s_fail_next_job_alloc = 0;
void mcp_http_test_fail_next_job_alloc(int n) {
    s_fail_next_job_alloc = n;
}
#endif

HttpMCPServer::HttpMCPServer(uint16_t port, const String& name, const String& version, const String& instructions)
    : MCPServerBase(name, version, instructions), port(port) {
    server = new AsyncWebServer(port);
}

HttpMCPServer::~HttpMCPServer() {
    /* Delete the server first so async_tcp stops feeding job_queue, then join
     * the worker. Like the AsyncWebServer it wraps, destruction is only safe
     * once no request is in flight. */
    if (server) {
        delete server;
        server = nullptr;
    }
    stopWorker();
}

bool HttpMCPServer::begin() {
    if (started) {
        return true;
    }
    if (!server) {
        return false;
    }

    // Best-effort: on failure tools/call degrades to inline execution.
    startWorker();
    setupWebServer();
    setupMDNS();
    started = true;
    return true;
}

bool HttpMCPServer::startWorker() {
    job_queue = xQueueCreate(MCP_HTTP_JOB_QUEUE_DEPTH, sizeof(HttpToolJob*));
    if (!job_queue) {
        Serial.println("[MCP_HTTP] Failed to create job queue; tool calls run inline");
        return false;
    }
    worker_done = xSemaphoreCreateBinary();
    if (!worker_done) {
        Serial.println("[MCP_HTTP] Failed to create worker semaphore; tool calls run inline");
        vQueueDelete(job_queue);
        job_queue = nullptr;
        return false;
    }
    worker_exit.store(false, std::memory_order_relaxed);
    if (xTaskCreate(HttpMCPServer::workerEntry, "mcp_http_worker", MCP_HTTP_WORKER_STACK_SIZE, this, 1,
                    &worker_handle) != pdPASS) {
        Serial.println("[MCP_HTTP] Failed to create worker task; tool calls run inline");
        vSemaphoreDelete(worker_done);
        worker_done = nullptr;
        vQueueDelete(job_queue);
        job_queue = nullptr;
        worker_handle = nullptr;
        return false;
    }
    return true;
}

void HttpMCPServer::stopWorker() {
    if (worker_handle) {
        /* Same join protocol as the BLE worker: raise the flag, then wake the
         * task. A 0-tick send into a full queue is fine — a full queue means
         * the worker has work queued and re-checks worker_exit per dequeue. */
        worker_exit.store(true, std::memory_order_release);
        HttpToolJob* sentinel = nullptr;
        if (job_queue) {
            xQueueSend(job_queue, &sentinel, 0);
        }
        if (worker_done) {
            xSemaphoreTake(worker_done, portMAX_DELAY);
        }
        worker_handle = nullptr;
    }

    if (job_queue) {
        HttpToolJob* job = nullptr;
        while (xQueueReceive(job_queue, &job, 0) == pdTRUE) {
            HttpToolJob::release(job);  // undelivered job; any live response filler still owns its ref
        }
        vQueueDelete(job_queue);
        job_queue = nullptr;
    }

    if (worker_done) {
        vSemaphoreDelete(worker_done);
        worker_done = nullptr;
    }
}

void HttpMCPServer::workerEntry(void* ctx) {
    auto* self = static_cast<HttpMCPServer*>(ctx);
    HttpToolJob* job = nullptr;
    for (;;) {
        if (xQueueReceive(self->job_queue, &job, portMAX_DELAY) == pdTRUE) {
            if (self->worker_exit.load(std::memory_order_acquire)) {
                HttpToolJob::release(job);  // sentinel (null) or an undelivered job
                break;
            }
            if (job) {
                MCPResponse mcpResponse = self->handle(job->request);
                job->response = self->serializeResponse(mcpResponse);
                job->done.store(true, std::memory_order_release);
                HttpToolJob::release(job);
                job = nullptr;
            }
        }
    }
    if (self->worker_done) {
        xSemaphoreGive(self->worker_done);
    }
    vTaskDelete(NULL);
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

/* For transport-level rejections where no request id is known (pre-parse
 * paths only) — the JSON-RPC id is null, as the spec requires when the id
 * could not be detected. Once a request has been parsed, build the error
 * with createJSONRPCError(request.id()) instead so the id is echoed. */
void HttpMCPServer::sendJSONRPCError(AsyncWebServerRequest* request, int httpCode, ErrorCode rpcCode,
                                     const char* message) {
    JsonDocument nullId;
    nullId.set(nullptr);
    MCPResponse error = createJSONRPCError(httpCode, static_cast<int>(rpcCode), nullId.as<JsonVariantConst>(), message);
    sendMCPResponse(request, error);
}

void HttpMCPServer::handlePostComplete(AsyncWebServerRequest* request) {
    BodyBuffer* body = static_cast<BodyBuffer*>(request->_tempObject);

    if (!isJsonContentType(request->contentType())) {
        if (body) {
            free(body);
            request->_tempObject = nullptr;
        }
        sendJSONRPCError(request, 415, ErrorCode::INVALID_REQUEST, "Content-Type must be application/json");
        return;
    }

    if (!body) {
        if (request->contentLength() == 0) {
            // No body at all; let the parser produce the JSON-RPC parse error.
            handleJsonBody(request, "");
            return;
        }
        // The body callback could not allocate the accumulation buffer.
        sendJSONRPCError(request, 500, ErrorCode::INTERNAL_ERROR, "Out of memory");
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

    /* tools/call runs user code of unknown duration, so it executes on the
     * worker task and is answered with a deferred chunked response. Everything
     * else — protocol methods, notifications, malformed requests — is pure
     * in-memory JSON work and stays inline. */
    if (worker_handle && mcpReq.method == "tools/call" && !mcpReq.isNotification()) {
        deferToolCall(request, std::move(mcpReq));
        return;
    }

    MCPResponse mcpRes = handle(mcpReq);
    sendMCPResponse(request, mcpRes);
}

void HttpMCPServer::deferToolCall(AsyncWebServerRequest* request, MCPRequest&& mcpRequest) {
    /* Deferred responses rely on chunked framing, which needs HTTP/1.1. The
     * pinned ESPAsyncWebServer 1.2.4 would cope with a version-0 request on
     * its own — beginChunkedResponse (`if(_version)`, WebRequest.cpp) routes
     * it to a non-chunked AsyncCallbackResponse whose body is close-delimited
     * with no chunk framing (_chunked stays false) — but this library compiles
     * against whichever fork __has_include finds, and that fallback differs
     * across forks and is untested on hardware. Rejecting explicitly is safer
     * than trusting it. Inline methods (initialize, tools/list, ping) still
     * serve HTTP/1.0. */
    if (request->version() == 0) {
        MCPResponse unsupported =
            createJSONRPCError(505, static_cast<int>(ErrorCode::SERVER_ERROR), mcpRequest.id(),
                               "HTTP/1.1 required for deferred tool calls");
        sendMCPResponse(request, unsupported);
        return;
    }

    HttpToolJob* job = nullptr;
#ifdef MCP_HTTP_TEST_HOOKS
    if (s_fail_next_job_alloc > 0) {
        s_fail_next_job_alloc--;
    } else {
        job = new (std::nothrow) HttpToolJob();
    }
#else
    job = new (std::nothrow) HttpToolJob();
#endif
    if (!job) {
        /* Graceful in both exception modes: a client hammering a device that
         * is out of memory gets 500s, not reboots. The request is parsed by
         * now, so echo its id — sendJSONRPCError is for pre-parse rejections
         * and would answer id:null, which strict clients cannot correlate. */
        MCPResponse oom = createJSONRPCError(500, static_cast<int>(ErrorCode::INTERNAL_ERROR), mcpRequest.id(),
                                             "Out of memory");
        sendMCPResponse(request, oom);
        return;
    }
    job->request = std::move(mcpRequest);

    job->refs.fetch_add(1, std::memory_order_relaxed);  // the queue/worker reference
    if (xQueueSend(job_queue, &job, 0) != pdTRUE) {
        HttpToolJob::release(job);  // the hand-off that never happened
        /* Answer right away rather than queueing without bound. 200 + JSON-RPC
         * error keeps the failure parseable by MCP SDKs (a bare 503 would
         * surface as an opaque transport error). */
        MCPResponse busy = createJSONRPCError(200, static_cast<int>(ErrorCode::SERVER_ERROR), job->request.id(),
                                              "Server busy: tool call queue is full");
        sendMCPResponse(request, busy);
        HttpToolJob::release(job);  // the creator reference; frees the job
        return;
    }

    JobRef ref(job);  // adopts the creator reference

    /* Fast path. The deferred reply below can only be written when the filler
     * next runs, and that is driven by tcp_poll at one lwIP coarse tick —
     * ~500 ms. Most tools (read a pin, read a sensor) finish in single-digit
     * milliseconds, so without this a 2 ms tool still answered in half a
     * second. Giving the worker a brief bounded window and replying inline when
     * it lands removes that floor for the common case.
     *
     * This does block async_tcp, which is exactly what the worker exists to
     * avoid — but bounded by a handful of milliseconds, not by an arbitrary
     * handler. Set MCP_HTTP_FAST_PATH_WAIT_MS to 0 to opt out entirely and
     * always defer. */
#if MCP_HTTP_FAST_PATH_WAIT_MS > 0
    for (uint32_t waited = 0; waited < MCP_HTTP_FAST_PATH_WAIT_MS; waited++) {
        if (ref->done.load(std::memory_order_acquire)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (ref->done.load(std::memory_order_acquire)) {
        /* Plain, length-delimited response: no chunk framing, and one fewer
         * round of filler callbacks. hasBody() is implied — tools/call always
         * produces one, and notifications never reach deferToolCall. */
        AsyncWebServerResponse* inlineResponse =
            request->beginResponse(200, "application/json", ref->response.c_str());
        inlineResponse->addHeader("MCP-Protocol-Version", PROTOCOL_VERSION);
        request->send(inlineResponse);
        return;
    }
#endif

    /* The filler runs on the async_tcp task whenever the TCP window opens or
     * the connection polls (~500 ms). It captures only the job — never `this` —
     * so a response outliving the server cannot touch freed state. Returning
     * RESPONSE_TRY_AGAIN until the worker finishes keeps the connection open
     * without blocking; ESPAsyncWebServer already disables the 3 s RX idle
     * timeout for the connection once send() is called. Returning 0 emits the
     * terminating chunk. */
    AsyncWebServerResponse* httpResponse = request->beginChunkedResponse(
        "application/json", [ref](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            if (!ref->done.load(std::memory_order_acquire)) {
                return RESPONSE_TRY_AGAIN;
            }
            const std::string& payload = ref->response;
            if (index >= payload.size()) {
                return 0;
            }
            size_t n = payload.size() - index;
            if (n > maxLen) {
                n = maxLen;
            }
            memcpy(buffer, payload.data() + index, n);
            return n;
        });
    httpResponse->addHeader("MCP-Protocol-Version", PROTOCOL_VERSION);
    request->send(httpResponse);
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
