#ifndef HTTP_MCP_SERVER_H
#define HTTP_MCP_SERVER_H

#if __has_include(<ESPAsyncWebServer.h>)

#include "MCPServer.h"
#include <ESPAsyncWebServer.h>

// Upper bound on an accepted POST body. Larger requests are rejected with
// HTTP 413 before any buffer is allocated, so a hostile or buggy client cannot
// exhaust the heap by declaring a huge Content-Length.
#ifndef MCP_HTTP_MAX_BODY_SIZE
#define MCP_HTTP_MAX_BODY_SIZE 8192
#endif

class HttpMCPServer : public MCPServerBase {
   public:
    // Construct only after WiFi is connected: setupMDNS() reads WiFi.localIP() to publish the
    // `endpoint` TXT record, so a pre-connect construction would advertise 0.0.0.0.
    HttpMCPServer(uint16_t port, const String& name = DEFAULT_SERVER_NAME,
                  const String& version = DEFAULT_SERVER_VERSION,
                  const String& instructions = "");
    ~HttpMCPServer();

   private:
    void setupWebServer();
    void setupMDNS();
    void handlePostComplete(AsyncWebServerRequest* request);
    void handleJsonBody(AsyncWebServerRequest* request, const char* body);
    void sendJSONRPCError(AsyncWebServerRequest* request, int httpCode, ErrorCode rpcCode, const char* message);
    void sendMCPResponse(AsyncWebServerRequest* request, const MCPResponse& response);
    bool validateProtocolVersionHeader(AsyncWebServerRequest* request, const MCPRequest& mcpRequest);
    bool validateOriginHeader(AsyncWebServerRequest* request);
    std::string generateUUID();

    AsyncWebServer* server;
    uint16_t port;
};

#endif  // __has_include(<ESPAsyncWebServer.h>)
#endif  // HTTP_MCP_SERVER_H
