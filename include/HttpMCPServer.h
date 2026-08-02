#ifndef HTTP_MCP_SERVER_H
#define HTTP_MCP_SERVER_H

#if __has_include(<ESPAsyncWebServer.h>)

#include "MCPServer.h"
#include <ESPAsyncWebServer.h>

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Upper bound on an accepted POST body. Larger requests are rejected with
// HTTP 413 before any buffer is allocated, so a hostile or buggy client cannot
// exhaust the heap by declaring a huge Content-Length.
#ifndef MCP_HTTP_MAX_BODY_SIZE
#define MCP_HTTP_MAX_BODY_SIZE 8192
#endif

// Depth of the tools/call hand-off queue between the async_tcp task and the
// HTTP worker task. A full queue answers new tool calls immediately with
// JSON-RPC -32000 ("server busy") instead of buffering without bound.
#ifndef MCP_HTTP_JOB_QUEUE_DEPTH
#define MCP_HTTP_JOB_QUEUE_DEPTH 4
#endif

// Stack size of the HTTP worker task that runs tool handlers.
#ifndef MCP_HTTP_WORKER_STACK_SIZE
#define MCP_HTTP_WORKER_STACK_SIZE 8192
#endif

// How long a tools/call may be waited on inline before falling back to the
// deferred chunked reply. The deferred path can only be written when the
// connection next polls (one lwIP coarse tick, ~500 ms), so without a short
// wait even a 2 ms tool answers in half a second. The cost is blocking
// async_tcp for at most this long. Set to 0 to always defer.
#ifndef MCP_HTTP_FAST_PATH_WAIT_MS
#define MCP_HTTP_FAST_PATH_WAIT_MS 20
#endif

#ifdef MCP_HTTP_TEST_HOOKS
/* Test-only: force the next `n` deferred-job allocations to fail (simulate
 * OOM). Compiled out of production builds. */
void mcp_http_test_fail_next_job_alloc(int n);
#endif

class HttpMCPServer : public MCPServerBase {
   public:
    // Call begin() only after WiFi is connected: setupMDNS() reads
    // WiFi.localIP() to publish the endpoint TXT record.
    HttpMCPServer(uint16_t port, const String& name = DEFAULT_SERVER_NAME,
                  const String& version = DEFAULT_SERVER_VERSION,
                  const String& instructions = "");
    ~HttpMCPServer();

    // Register all tools before calling begin(). Starting explicitly prevents
    // request handlers from racing mutations of the tool registry.
    bool begin();

   private:
    void setupWebServer();
    void setupMDNS();
    void handlePostComplete(AsyncWebServerRequest* request);
    void handleJsonBody(AsyncWebServerRequest* request, const char* body);
    void deferToolCall(AsyncWebServerRequest* request, MCPRequest&& mcpRequest);
    void sendJSONRPCError(AsyncWebServerRequest* request, int httpCode, ErrorCode rpcCode, const char* message);
    void sendMCPResponse(AsyncWebServerRequest* request, const MCPResponse& response);
    bool validateProtocolVersionHeader(AsyncWebServerRequest* request, const MCPRequest& mcpRequest);
    bool validateOriginHeader(AsyncWebServerRequest* request);
    std::string generateUUID();

    bool startWorker();
    void stopWorker();
    static void workerEntry(void* ctx);

    AsyncWebServer* server;
    uint16_t port;
    bool started = false;

    /* tools/call handlers run on this worker task, fed through job_queue, so a
     * slow or blocking handler never stalls the async_tcp task (which services
     * every TCP connection in the firmware). Null when startWorker() failed —
     * tools/call then degrades to inline execution. */
    QueueHandle_t job_queue = nullptr;
    TaskHandle_t worker_handle = nullptr;
    SemaphoreHandle_t worker_done = nullptr;
    std::atomic<bool> worker_exit{false};
};

#endif  // __has_include(<ESPAsyncWebServer.h>)
#endif  // HTTP_MCP_SERVER_H
