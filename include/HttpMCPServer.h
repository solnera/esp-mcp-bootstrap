#ifndef HTTP_MCP_SERVER_H
#define HTTP_MCP_SERVER_H

#if __has_include(<ESPAsyncWebServer.h>)

#include "MCPServer.h"
#include <ESPAsyncWebServer.h>

class HttpMCPServer : public MCPServerBase {
   public:
    HttpMCPServer(uint16_t port, const String& name = DEFAULT_SERVER_NAME,
                  const String& version = DEFAULT_SERVER_VERSION,
                  const String& instructions = "");
    ~HttpMCPServer();

   private:
    void setupWebServer();
    void setupMDNS();
    void handleJsonBody(AsyncWebServerRequest* request, const String& body);
    void sendMCPResponse(AsyncWebServerRequest* request, const MCPResponse& response);
    bool validateProtocolVersionHeader(AsyncWebServerRequest* request, const MCPRequest& mcpRequest);
    bool validateOriginHeader(AsyncWebServerRequest* request);
    std::string generateUUID();

    AsyncWebServer* server;
    uint16_t port;
};

#endif  // __has_include(<ESPAsyncWebServer.h>)
#endif  // HTTP_MCP_SERVER_H
