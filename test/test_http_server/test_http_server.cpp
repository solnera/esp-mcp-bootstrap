#include <unity.h>

#include <cstring>
#include <memory>
#include <string>

#include "HttpMCPServer.h"

/* Drives the handlers HttpMCPServer registers on the mock AsyncWebServer the
 * way the real library does: body callback per chunk, then the onRequest
 * callback once the request is complete. */

namespace {

class EchoHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        JsonDocument result;
        result["echo"] = params["text"];
        return result;
    }
};

struct TestServer {
    TestServer() : mcp(3000, "test-http", "1.0.0") {
        Tool tool;
        tool.name = "echo";
        tool.description = "Echo";
        tool.inputSchema.type = "object";
        tool.handler = std::make_shared<EchoHandler>();
        mcp.RegisterTool(tool);
        web = mock_async_web::lastServer();
    }

    const AsyncWebServer::Route* postRoute() const { return web->findRoute("/mcp", HTTP_POST); }

    HttpMCPServer mcp;
    AsyncWebServer* web = nullptr;
};

/* Feeds `body` through the POST route in `chunkSize`-byte pieces and finishes
 * the request. chunkSize 0 = one chunk. */
void drivePost(TestServer& srv, AsyncWebServerRequest& req, const std::string& body, size_t chunkSize = 0) {
    const AsyncWebServer::Route* route = srv.postRoute();
    TEST_ASSERT_NOT_NULL(route);

    req.setContentLength(body.size());
    if (!body.empty()) {
        const size_t step = chunkSize == 0 ? body.size() : chunkSize;
        for (size_t index = 0; index < body.size(); index += step) {
            size_t len = body.size() - index < step ? body.size() - index : step;
            route->onBody(&req, reinterpret_cast<uint8_t*>(const_cast<char*>(body.data() + index)), len, index,
                          body.size());
        }
    }
    route->onRequest(&req);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_tools_list_roundtrip(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"tools\""));
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"echo\""));
    TEST_ASSERT_EQUAL_STRING("2025-11-25", req.lastHeaders["MCP-Protocol-Version"].c_str());
    TEST_ASSERT_NULL(req._tempObject);
}

void test_tool_call_roundtrip_chunked_body(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req,
              R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})",
              7);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"hi\""));
}

void test_empty_body_post_gets_parse_error_response(void) {
    /* Previously the empty lambda in the onRequest slot meant a body-less POST
     * never got any response and the connection hung until timeout. */
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, "");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(400, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "-32700"));
}

void test_oversized_body_is_rejected_with_413(void) {
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    const size_t total = MCP_HTTP_MAX_BODY_SIZE + 1;
    req.setContentLength(total);

    /* Only a fraction of the declared body ever arrives; the limit must apply
     * to the declared Content-Length, before buffering the payload. */
    std::string chunk(64, 'X');
    route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), 0, total);
    route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), chunk.size(), total);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(413, req.lastCode);
    TEST_ASSERT_NULL(req._tempObject);
}

void test_chunked_upload_without_length_gets_411(void) {
    /* NOTE: this drives the total==0 body-callback shape as defense-in-depth.
     * ESPAsyncWebServer 1.2.x does not actually support chunked request bodies,
     * so on that library this branch is a fallback, not an observed behavior. */
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    req.setContentLength(0);
    std::string chunk = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), 0, 0);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(411, req.lastCode);
}

void test_body_overshooting_content_length_is_clamped_and_answered(void) {
    /* A client that sends more bytes than its declared Content-Length must
     * still get a response for the declared prefix — not hang. */
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    std::string body = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    req.setContentLength(body.size());
    route->onBody(&req, reinterpret_cast<uint8_t*>(body.data()), body.size(), 0, body.size());
    std::string extra = "GARBAGE";
    route->onBody(&req, reinterpret_cast<uint8_t*>(extra.data()), extra.size(), body.size(), body.size());
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"tools\""));
}

void test_undersized_body_gets_400_incomplete(void) {
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    req.setContentLength(100);
    std::string chunk(40, 'X');
    route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), 0, 100);
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(400, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "Incomplete"));
}

void test_non_json_content_type_gets_415(void) {
    /* ESPAsyncWebServer consumes urlencoded/multipart bodies itself, so our
     * body callback never runs; the server must not misreport that as OOM. */
    TestServer srv;
    AsyncWebServerRequest req;

    const AsyncWebServer::Route* route = srv.postRoute();
    req.setContentLength(24);
    req.setContentType("application/x-www-form-urlencoded");
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(415, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "application/json"));
}

void test_aborted_upload_is_reclaimed_by_free(void) {
    /* The request dies mid-upload: onRequest never runs and the destructor
     * releases _tempObject with free(), as the real library does. The buffer
     * must be a plain malloc block for that to be leak- and corruption-free
     * (a String stored there used to leak its heap buffer). */
    TestServer srv;
    {
        AsyncWebServerRequest req;
        const AsyncWebServer::Route* route = srv.postRoute();
        req.setContentLength(4096);
        std::string chunk(512, 'A');
        route->onBody(&req, reinterpret_cast<uint8_t*>(chunk.data()), chunk.size(), 0, 4096);
        TEST_ASSERT_NOT_NULL(req._tempObject);
    }
    TEST_ASSERT_TRUE(true);  // reaching here without ASAN/heap errors is the assertion
}

void test_protocol_version_2025_06_18_header_is_accepted(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("MCP-Protocol-Version", "2025-06-18");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"tools\""));
}

void test_unsupported_protocol_version_header_is_rejected(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("MCP-Protocol-Version", "1999-01-01");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(400, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "-32602"));
}

void test_mismatched_origin_is_rejected(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("Origin", "http://evil.example");
    req.setHeader("Host", "192.168.4.1:3000");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(403, req.lastCode);
}

void test_matching_origin_is_accepted(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    req.setHeader("Origin", "http://192.168.4.1:3000");
    req.setHeader("Host", "192.168.4.1:3000");
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
}

void test_notification_returns_202_without_body(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");

    TEST_ASSERT_EQUAL_INT(1, req.responseCount);
    TEST_ASSERT_EQUAL_INT(202, req.lastCode);
    TEST_ASSERT_EQUAL_STRING("", req.lastBody.c_str());
}

void test_ping_over_http(void) {
    TestServer srv;
    AsyncWebServerRequest req;
    drivePost(srv, req, R"({"jsonrpc":"2.0","id":7,"method":"ping"})");

    TEST_ASSERT_EQUAL_INT(200, req.lastCode);
    TEST_ASSERT_NOT_NULL(strstr(req.lastBody.c_str(), "\"result\":{}"));
}

void test_get_mcp_returns_405(void) {
    TestServer srv;
    const AsyncWebServer::Route* route = srv.web->findRoute("/mcp", HTTP_GET);
    TEST_ASSERT_NOT_NULL(route);

    AsyncWebServerRequest req;
    route->onRequest(&req);

    TEST_ASSERT_EQUAL_INT(405, req.lastCode);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_tools_list_roundtrip);
    RUN_TEST(test_tool_call_roundtrip_chunked_body);
    RUN_TEST(test_empty_body_post_gets_parse_error_response);
    RUN_TEST(test_oversized_body_is_rejected_with_413);
    RUN_TEST(test_chunked_upload_without_length_gets_411);
    RUN_TEST(test_body_overshooting_content_length_is_clamped_and_answered);
    RUN_TEST(test_undersized_body_gets_400_incomplete);
    RUN_TEST(test_non_json_content_type_gets_415);
    RUN_TEST(test_aborted_upload_is_reclaimed_by_free);
    RUN_TEST(test_protocol_version_2025_06_18_header_is_accepted);
    RUN_TEST(test_unsupported_protocol_version_header_is_rejected);
    RUN_TEST(test_mismatched_origin_is_rejected);
    RUN_TEST(test_matching_origin_is_accepted);
    RUN_TEST(test_notification_returns_202_without_body);
    RUN_TEST(test_ping_over_http);
    RUN_TEST(test_get_mcp_returns_405);
    return UNITY_END();
}
