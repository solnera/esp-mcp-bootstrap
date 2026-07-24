#include <unity.h>
#include <ArduinoJson.h>
#include <cstring>
#include "MCPServer.h"

/* ======== Test Helpers ======== */

class TestMCPServer : public MCPServerBase {
public:
    using MCPServerBase::MCPServerBase;
    using MCPServerBase::parseRequest;
    using MCPServerBase::serializeResponse;
    using MCPServerBase::handle;
    using MCPServerBase::createJSONRPCError;
    using MCPServerBase::tools;
};

class EchoHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        JsonDocument result;
        result["echo"] = params["message"];
        return result;
    }
};

class ComplexHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        JsonDocument result;
        result["status"] = "ok";
        JsonArray items = result["items"].to<JsonArray>();
        items.add(1);
        items.add(2);
        items.add(3);
        JsonObject nested = result["nested"].to<JsonObject>();
        nested["key"] = "value";
        return result;
    }
};

class EmptyHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        (void)params;
        JsonDocument result;
        return result;
    }
};

class FailingHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        bool ignored;
        return call(params, ignored);
    }
    JsonDocument call(JsonVariantConst params, bool& isError) override {
        (void)params;
        isError = true;
        JsonDocument result;
        result["error"] = "sensor offline";
        return result;
    }
};

class ScalarHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        (void)params;
        JsonDocument result;
        result.set(42);
        return result;
    }
};

class ParamInspectHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        JsonDocument result;
        /* Echo back the number of top-level keys */
        int count = 0;
        for (JsonPairConst kv : params.as<JsonObjectConst>()) {
            (void)kv;
            count++;
        }
        result["param_count"] = count;
        return result;
    }
};

static TestMCPServer* server;

void setUp(void) {
    server = new TestMCPServer("TestServer", "1.0.0", "Test instructions");
}

void tearDown(void) {
    delete server;
    server = nullptr;
}

/* ======== parseRequest ======== */

void test_parse_valid_request(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");

    TEST_ASSERT_EQUAL_STRING("initialize", req.method.c_str());
    TEST_ASSERT_EQUAL(1, req.id().as<int>());
    TEST_ASSERT_TRUE(req.hasParams());
}

void test_parse_string_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":"abc-123","method":"tools/list"})");

    TEST_ASSERT_EQUAL_STRING("tools/list", req.method.c_str());
    TEST_ASSERT_EQUAL_STRING("abc-123", req.id().as<const char*>());
}

void test_parse_invalid_json(void) {
    MCPRequest req = server->parseRequest("{invalid}");
    TEST_ASSERT_EQUAL_STRING("", req.method.c_str());
}

void test_parse_empty_string(void) {
    MCPRequest req = server->parseRequest("");
    TEST_ASSERT_EQUAL_STRING("", req.method.c_str());
}

void test_parse_no_params(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");

    TEST_ASSERT_EQUAL_STRING("tools/list", req.method.c_str());
    TEST_ASSERT_FALSE(req.hasParams());
}

void test_parse_with_arguments(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hello"}}})");

    TEST_ASSERT_EQUAL_STRING("tools/call", req.method.c_str());
    TEST_ASSERT_TRUE(req.hasParams());
    TEST_ASSERT_EQUAL_STRING("echo", req.params()["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("hello", req.params()["arguments"]["message"].as<const char*>());
}

/* ======== handle: initialize ======== */

void test_handle_initialize(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());

    JsonVariantConst r = res.result();
    TEST_ASSERT_EQUAL_STRING("2025-11-25", r["protocolVersion"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("TestServer", r["serverInfo"]["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("1.0.0", r["serverInfo"]["version"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Test instructions", r["instructions"].as<const char*>());
    TEST_ASSERT_FALSE(r["capabilities"]["tools"]["listChanged"].as<bool>());
}

void test_handle_initialize_negotiates_supported_legacy_version(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_EQUAL_STRING("2024-11-05", res.result()["protocolVersion"].as<const char*>());
}

void test_handle_initialize_falls_back_to_latest_for_unknown_version(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2099-01-01"}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_EQUAL_STRING("2025-11-25", res.result()["protocolVersion"].as<const char*>());
}

void test_handle_initialize_no_instructions(void) {
    TestMCPServer srv("Srv", "2.0.0", "");
    MCPRequest req = srv.parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    MCPResponse res = srv.handle(req);

    TEST_ASSERT_TRUE(res.result()["instructions"].isNull());
}

/* ======== handle: notifications/initialized ======== */

void test_handle_notifications_initialized(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(202, res.code);
    TEST_ASSERT_FALSE(res.hasBody());
    std::string json = server->serializeResponse(res);
    TEST_ASSERT_EQUAL_STRING("", json.c_str());
}

/* ======== handle: tools/list ======== */

void test_handle_tools_list_empty(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_EQUAL(0, res.result()["tools"].as<JsonArrayConst>().size());
}

void test_handle_tools_list_with_tool(void) {
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo tool";
    tool.inputSchema.type = "object";

    Properties msgProp;
    msgProp.type = "string";
    msgProp.description = "Message to echo";
    tool.inputSchema.properties["message"] = msgProp;
    tool.inputSchema.required.push_back("message");
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    JsonArrayConst tools = res.result()["tools"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(1, tools.size());
    TEST_ASSERT_EQUAL_STRING("echo", tools[0]["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Echo tool", tools[0]["description"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("object", tools[0]["inputSchema"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string",
        tools[0]["inputSchema"]["properties"]["message"]["type"].as<const char*>());
}

void test_handle_tools_list_multiple_tools(void) {
    Tool t1;
    t1.name = "tool_a";
    t1.description = "A";
    t1.inputSchema.type = "object";
    t1.handler = std::make_shared<EchoHandler>();

    Tool t2;
    t2.name = "tool_b";
    t2.description = "B";
    t2.inputSchema.type = "object";
    t2.handler = std::make_shared<EchoHandler>();

    server->RegisterTool(t1);
    server->RegisterTool(t2);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(2, res.result()["tools"].as<JsonArrayConst>().size());
}

void test_handle_tools_list_with_output_schema(void) {
    Tool tool;
    tool.name = "typed";
    tool.description = "Typed tool";
    tool.inputSchema.type = "object";
    tool.outputSchema.type = "object";

    Properties outProp;
    outProp.type = "string";
    tool.outputSchema.properties["result"] = outProp;
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    JsonVariantConst t = res.result()["tools"][0];
    TEST_ASSERT_EQUAL_STRING("object", t["outputSchema"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string",
        t["outputSchema"]["properties"]["result"]["type"].as<const char*>());
}

/* ======== handle: tools/call ======== */

void test_handle_tool_call_success(void) {
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hello"}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());

    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(1, content.size());
    TEST_ASSERT_EQUAL_STRING("text", content[0]["type"].as<const char*>());

    /* Parse the text field (serialized JSON from handler) */
    JsonDocument textDoc;
    deserializeJson(textDoc, content[0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("hello", textDoc["echo"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("hello", res.result()["structuredContent"]["echo"].as<const char*>());
    TEST_ASSERT_FALSE(res.result()["isError"].as<bool>());
}

void test_handle_tool_call_handler_reports_execution_error(void) {
    Tool tool;
    tool.name = "flaky";
    tool.description = "Fails at runtime";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<FailingHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"flaky","arguments":{}}})");
    MCPResponse res = server->handle(req);

    /* Per MCP, an execution failure is a RESULT with isError=true so the LLM
     * can see it — not a JSON-RPC protocol error. */
    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());
    TEST_ASSERT_TRUE(res.result()["isError"].as<bool>());

    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(1, content.size());
    TEST_ASSERT_EQUAL_STRING("text", content[0]["type"].as<const char*>());
    JsonDocument textDoc;
    deserializeJson(textDoc, content[0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("sensor offline", textDoc["error"].as<const char*>());

    /* Error payloads would not conform to a declared outputSchema. */
    TEST_ASSERT_TRUE(res.result()["structuredContent"].isNull());
}

void test_handle_tool_call_scalar_result_has_no_structured_content(void) {
    Tool tool;
    tool.name = "scalar";
    tool.description = "Returns a bare number";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<ScalarHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"scalar","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.result()["isError"].as<bool>());
    /* The value still reaches the client as serialized text content... */
    TEST_ASSERT_EQUAL_STRING("42", res.result()["content"][0]["text"].as<const char*>());
    /* ...but structuredContent is defined as a JSON object by MCP, so a
     * non-object result must not be attached. */
    TEST_ASSERT_TRUE(res.result()["structuredContent"].isNull());
}

void test_handle_tool_call_missing_name(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32602, res.error()["code"].as<int>());
}

void test_handle_tool_call_unknown_tool(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"nonexistent","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    /* Per MCP, an unknown tool is -32602 invalid params, not -32601: the
     * method (tools/call) itself exists. */
    TEST_ASSERT_EQUAL(-32602, res.error()["code"].as<int>());
    TEST_ASSERT_NOT_NULL(strstr(res.error()["message"].as<const char*>(), "Unknown tool: nonexistent"));
}

void test_handle_tool_call_null_handler(void) {
    Tool tool;
    tool.name = "broken";
    tool.description = "Broken";
    tool.inputSchema.type = "object";
    tool.handler = nullptr;
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"broken","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32603, res.error()["code"].as<int>());
}

/* ======== handle: unknown method & parse error ======== */

void test_handle_unknown_method(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"unknown/method"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32601, res.error()["code"].as<int>());
}

void test_handle_parse_error(void) {
    MCPRequest req = server->parseRequest("{bad json}");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32700, res.error()["code"].as<int>());
}

/* ======== serializeResponse ======== */

void test_serialize_response_with_result(void) {
    MCPResponse res;
    res.idDoc.set(1);
    res.resultDoc["value"] = "test";

    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
    TEST_ASSERT_EQUAL(1, doc["id"].as<int>());
    TEST_ASSERT_EQUAL_STRING("test", doc["result"]["value"].as<const char*>());
    TEST_ASSERT_TRUE(doc["error"].isNull());
}

void test_serialize_response_with_error(void) {
    JsonDocument idDoc;
    idDoc.set(42);
    MCPResponse res = server->createJSONRPCError(
        400, -32700, idDoc.as<JsonVariantConst>(), "Parse error");

    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL(42, doc["id"].as<int>());
    TEST_ASSERT_EQUAL(-32700, doc["error"]["code"].as<int>());
    TEST_ASSERT_EQUAL_STRING("Parse error", doc["error"]["message"].as<const char*>());
}

void test_serialize_includes_jsonrpc_version(void) {
    MCPResponse res;
    res.idDoc.set(99);

    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
}

/* ======== RegisterTool ======== */

void test_register_tool(void) {
    Tool tool;
    tool.name = "test_tool";
    tool.description = "A test tool";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<EchoHandler>();

    server->RegisterTool(tool);

    TEST_ASSERT_EQUAL(1, server->tools.size());
    TEST_ASSERT_TRUE(server->tools.find("test_tool") != server->tools.end());
}

void test_register_overwrites_same_name(void) {
    Tool t1;
    t1.name = "tool";
    t1.description = "first";
    t1.inputSchema.type = "object";
    t1.handler = std::make_shared<EchoHandler>();

    Tool t2;
    t2.name = "tool";
    t2.description = "second";
    t2.inputSchema.type = "object";
    t2.handler = std::make_shared<EchoHandler>();

    server->RegisterTool(t1);
    server->RegisterTool(t2);

    TEST_ASSERT_EQUAL(1, server->tools.size());

    /* Verify it's the second registration */
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);
    TEST_ASSERT_EQUAL_STRING("second",
        res.result()["tools"][0]["description"].as<const char*>());
}

/* ======== Properties serialization ======== */

void test_properties_basic_type(void) {
    Properties p;
    p.type = "object";

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    TEST_ASSERT_EQUAL_STRING("object", doc["type"].as<const char*>());
}

void test_properties_with_title_desc(void) {
    Properties p;
    p.type = "string";
    p.title = "Name";
    p.description = "User name";

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    TEST_ASSERT_EQUAL_STRING("string", doc["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Name", doc["title"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("User name", doc["description"].as<const char*>());
}

void test_properties_nested_with_required(void) {
    Properties root;
    root.type = "object";

    Properties child;
    child.type = "string";
    child.description = "Name field";
    root.properties["name"] = child;
    root.required.push_back("name");

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    root.toJson(obj);

    TEST_ASSERT_EQUAL_STRING("string",
        doc["properties"]["name"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL(1, doc["required"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("name", doc["required"][0].as<const char*>());
}

void test_properties_enum(void) {
    Properties p;
    p.type = "string";
    p.enumValues.push_back("red");
    p.enumValues.push_back("green");
    p.enumValues.push_back("blue");

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    JsonArrayConst arr = doc["enum"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(3, arr.size());
    TEST_ASSERT_EQUAL_STRING("red", arr[0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("green", arr[1].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("blue", arr[2].as<const char*>());
}

void test_properties_array_items(void) {
    Properties p;
    p.type = "array";
    p.items.reset(new Properties());
    p.items->type = "string";

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    TEST_ASSERT_EQUAL_STRING("array", doc["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string", doc["items"]["type"].as<const char*>());
}

void test_properties_additional_properties_false(void) {
    Properties p;
    p.type = "object";
    p.hasAdditionalProperties = true;
    p.additionalProperties = false;

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    TEST_ASSERT_FALSE(doc["additionalProperties"].as<bool>());
}

void test_properties_format(void) {
    Properties p;
    p.type = "string";
    p.format = "date-time";

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    TEST_ASSERT_EQUAL_STRING("date-time", doc["format"].as<const char*>());
}

void test_properties_default_value(void) {
    Properties p;
    p.type = "string";
    p.defaultValue = "hello";

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    TEST_ASSERT_EQUAL_STRING("hello", doc["default"].as<const char*>());
}

void test_properties_one_of(void) {
    Properties p;
    p.type = "object";

    Properties opt1;
    opt1.type = "string";
    Properties opt2;
    opt2.type = "number";
    p.oneOf.push_back(opt1);
    p.oneOf.push_back(opt2);

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    JsonArrayConst arr = doc["oneOf"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(2, arr.size());
    TEST_ASSERT_EQUAL_STRING("string", arr[0]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("number", arr[1]["type"].as<const char*>());
}

void test_properties_any_of(void) {
    Properties p;
    p.type = "object";

    Properties opt;
    opt.type = "integer";
    p.anyOf.push_back(opt);

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    TEST_ASSERT_EQUAL(1, doc["anyOf"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("integer", doc["anyOf"][0]["type"].as<const char*>());
}

void test_properties_all_of(void) {
    Properties p;
    p.type = "object";

    Properties opt;
    opt.type = "boolean";
    p.allOf.push_back(opt);

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    TEST_ASSERT_EQUAL(1, doc["allOf"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("boolean", doc["allOf"][0]["type"].as<const char*>());
}

/* ======== Tool::toString ======== */

void test_tool_to_string(void) {
    Tool t;
    t.name = "mytool";
    t.description = "My tool";
    t.inputSchema.type = "object";

    String s = t.toString();

    JsonDocument doc;
    deserializeJson(doc, s.c_str());
    TEST_ASSERT_EQUAL_STRING("mytool", doc["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("My tool", doc["description"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("object", doc["inputSchema"]["type"].as<const char*>());
}

void test_tool_to_string_with_output_schema(void) {
    Tool t;
    t.name = "typed";
    t.description = "Typed";
    t.inputSchema.type = "object";
    t.outputSchema.type = "string";

    String s = t.toString();

    JsonDocument doc;
    deserializeJson(doc, s.c_str());
    TEST_ASSERT_EQUAL_STRING("string", doc["outputSchema"]["type"].as<const char*>());
}

/* ======== MCPResponse struct ======== */

void test_response_default_constructor(void) {
    MCPResponse res;
    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_FALSE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());
}

void test_response_id_constructor(void) {
    JsonDocument id;
    id.set(42);
    MCPResponse res(id.as<JsonVariantConst>());

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_EQUAL(42, res.id().as<int>());
}

void test_response_code_id_constructor(void) {
    JsonDocument id;
    id.set("req-1");
    MCPResponse res(404, id.as<JsonVariantConst>());

    TEST_ASSERT_EQUAL(404, res.code);
    TEST_ASSERT_EQUAL_STRING("req-1", res.id().as<const char*>());
}

/* ======== MCPRequest struct ======== */

void test_request_default_state(void) {
    MCPRequest req;
    TEST_ASSERT_EQUAL_STRING("", req.method.c_str());
    TEST_ASSERT_FALSE(req.hasParams());
    TEST_ASSERT_TRUE(req.id().isNull());
}

/* ======== Properties deep copy ======== */

void test_properties_copy_constructor_deep(void) {
    Properties original;
    original.type = "object";
    original.title = "Root";
    original.items.reset(new Properties());
    original.items->type = "string";
    original.items->description = "inner";

    Properties child;
    child.type = "integer";
    original.properties["count"] = child;
    original.required.push_back("count");
    original.enumValues.push_back("a");
    original.format = "uri";
    original.defaultValue = "default";
    original.hasAdditionalProperties = true;
    original.additionalProperties = false;

    Properties copy(original);

    /* Verify deep copy of items */
    TEST_ASSERT_NOT_NULL(copy.items.get());
    TEST_ASSERT_EQUAL_STRING("string", copy.items->type.c_str());
    TEST_ASSERT_EQUAL_STRING("inner", copy.items->description.c_str());
    /* Verify it's a separate pointer */
    TEST_ASSERT_NOT_EQUAL(original.items.get(), copy.items.get());

    /* Verify other fields copied */
    TEST_ASSERT_EQUAL_STRING("object", copy.type.c_str());
    TEST_ASSERT_EQUAL_STRING("Root", copy.title.c_str());
    TEST_ASSERT_EQUAL(1, copy.properties.size());
    TEST_ASSERT_EQUAL_STRING("integer", copy.properties["count"].type.c_str());
    TEST_ASSERT_EQUAL(1, copy.required.size());
    TEST_ASSERT_EQUAL(1, copy.enumValues.size());
    TEST_ASSERT_EQUAL_STRING("uri", copy.format.c_str());
    TEST_ASSERT_EQUAL_STRING("default", copy.defaultValue.c_str());
    TEST_ASSERT_TRUE(copy.hasAdditionalProperties);
    TEST_ASSERT_FALSE(copy.additionalProperties);
}

void test_properties_assignment_deep(void) {
    Properties original;
    original.type = "array";
    original.items.reset(new Properties());
    original.items->type = "number";

    Properties copy;
    copy.type = "string"; /* overwritten by assignment */
    copy = original;

    TEST_ASSERT_EQUAL_STRING("array", copy.type.c_str());
    TEST_ASSERT_NOT_NULL(copy.items.get());
    TEST_ASSERT_EQUAL_STRING("number", copy.items->type.c_str());
    TEST_ASSERT_NOT_EQUAL(original.items.get(), copy.items.get());
}

void test_properties_self_assignment(void) {
    Properties p;
    p.type = "boolean";
    p.items.reset(new Properties());
    p.items->type = "integer";

    p = p; /* self-assignment */

    TEST_ASSERT_EQUAL_STRING("boolean", p.type.c_str());
    TEST_ASSERT_NOT_NULL(p.items.get());
    TEST_ASSERT_EQUAL_STRING("integer", p.items->type.c_str());
}

void test_properties_copy_null_items(void) {
    Properties original;
    original.type = "string";
    /* items is null */

    Properties copy(original);
    TEST_ASSERT_NULL(copy.items.get());
    TEST_ASSERT_EQUAL_STRING("string", copy.type.c_str());
}

/* ======== Properties::toString ======== */

void test_properties_to_string(void) {
    Properties p;
    p.type = "object";
    Properties child;
    child.type = "string";
    p.properties["name"] = child;

    String s = p.toString();
    JsonDocument doc;
    deserializeJson(doc, s.c_str());
    TEST_ASSERT_EQUAL_STRING("object", doc["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string", doc["properties"]["name"]["type"].as<const char*>());
}

/* ======== createJSONRPCError ======== */

void test_create_error_all_codes(void) {
    JsonDocument id;
    id.set(1);

    struct { int code; const char* name; } codes[] = {
        {-32700, "PARSE_ERROR"},
        {-32600, "INVALID_REQUEST"},
        {-32601, "METHOD_NOT_FOUND"},
        {-32602, "INVALID_PARAMS"},
        {-32603, "INTERNAL_ERROR"},
        {-32000, "SERVER_ERROR"},
    };

    for (auto& c : codes) {
        MCPResponse res = server->createJSONRPCError(400, c.code, id.as<JsonVariantConst>(), c.name);
        TEST_ASSERT_TRUE(res.hasError());
        TEST_ASSERT_EQUAL(c.code, res.error()["code"].as<int>());
        TEST_ASSERT_EQUAL_STRING(c.name, res.error()["message"].as<const char*>());
        TEST_ASSERT_EQUAL(400, res.code);
    }
}

void test_create_error_preserves_string_id(void) {
    JsonDocument id;
    id.set("request-abc");

    MCPResponse res = server->createJSONRPCError(500, -32000, id.as<JsonVariantConst>(), "Server error");
    TEST_ASSERT_EQUAL_STRING("request-abc", res.id().as<const char*>());
}

/* ======== tools/call edge cases ======== */

void test_handle_tool_call_empty_arguments(void) {
    Tool tool;
    tool.name = "inspect";
    tool.description = "Inspect params";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<ParamInspectHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"inspect","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());

    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
    JsonDocument textDoc;
    deserializeJson(textDoc, content[0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL(0, textDoc["param_count"].as<int>());
}

void test_handle_tool_call_complex_result(void) {
    Tool tool;
    tool.name = "complex";
    tool.description = "Returns complex data";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<ComplexHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"complex","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(1, content.size());
    TEST_ASSERT_EQUAL_STRING("text", content[0]["type"].as<const char*>());

    JsonDocument textDoc;
    deserializeJson(textDoc, content[0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("ok", textDoc["status"].as<const char*>());
    TEST_ASSERT_EQUAL(3, textDoc["items"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("value", textDoc["nested"]["key"].as<const char*>());
}

void test_handle_tool_call_empty_result(void) {
    Tool tool;
    tool.name = "empty";
    tool.description = "Returns nothing";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<EmptyHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"empty","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    /* Content should still have one text entry */
    JsonArrayConst content = res.result()["content"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(1, content.size());
}

void test_handle_tool_call_multiple_different_tools(void) {
    Tool echo;
    echo.name = "echo";
    echo.description = "Echo";
    echo.inputSchema.type = "object";
    echo.handler = std::make_shared<EchoHandler>();

    Tool complex;
    complex.name = "complex";
    complex.description = "Complex";
    complex.inputSchema.type = "object";
    complex.handler = std::make_shared<ComplexHandler>();

    server->RegisterTool(echo);
    server->RegisterTool(complex);

    /* Call echo */
    MCPRequest req1 = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hi"}}})");
    MCPResponse res1 = server->handle(req1);
    TEST_ASSERT_TRUE(res1.hasResult());
    JsonDocument doc1;
    deserializeJson(doc1, res1.result()["content"][0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("hi", doc1["echo"].as<const char*>());

    /* Call complex */
    MCPRequest req2 = server->parseRequest(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"complex","arguments":{}}})");
    MCPResponse res2 = server->handle(req2);
    TEST_ASSERT_TRUE(res2.hasResult());
    JsonDocument doc2;
    deserializeJson(doc2, res2.result()["content"][0]["text"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("ok", doc2["status"].as<const char*>());
}

/* ======== Full roundtrip: parse → handle → serialize ======== */

void test_roundtrip_initialize(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    MCPResponse res = server->handle(req);
    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
    TEST_ASSERT_EQUAL(1, doc["id"].as<int>());
    TEST_ASSERT_EQUAL_STRING("2025-11-25", doc["result"]["protocolVersion"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("TestServer", doc["result"]["serverInfo"]["name"].as<const char*>());
    TEST_ASSERT_TRUE(doc["error"].isNull());
}

void test_roundtrip_tool_call(void) {
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":"abc","method":"tools/call","params":{"name":"echo","arguments":{"message":"world"}}})");
    MCPResponse res = server->handle(req);
    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("abc", doc["id"].as<const char*>());
    TEST_ASSERT_TRUE(doc["error"].isNull());

    /* Verify nested result text */
    const char* text = doc["result"]["content"][0]["text"].as<const char*>();
    JsonDocument textDoc;
    deserializeJson(textDoc, text);
    TEST_ASSERT_EQUAL_STRING("world", textDoc["echo"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("world", doc["result"]["structuredContent"]["echo"].as<const char*>());
}

void test_roundtrip_error(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":99,"method":"nonexistent"})");
    MCPResponse res = server->handle(req);
    std::string json = server->serializeResponse(res);

    JsonDocument doc;
    deserializeJson(doc, json);
    TEST_ASSERT_EQUAL_STRING("2.0", doc["jsonrpc"].as<const char*>());
    TEST_ASSERT_EQUAL(99, doc["id"].as<int>());
    TEST_ASSERT_EQUAL(-32601, doc["error"]["code"].as<int>());
    TEST_ASSERT_TRUE(doc["result"].isNull());
}

/* ======== Default server name/version ======== */

void test_server_default_name_version(void) {
    TestMCPServer defaultServer;
    MCPRequest req = defaultServer.parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    MCPResponse res = defaultServer.handle(req);

    TEST_ASSERT_EQUAL_STRING("ESP32-MCP-Server", res.result()["serverInfo"]["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("1.0.0", res.result()["serverInfo"]["version"].as<const char*>());
    TEST_ASSERT_TRUE(res.result()["instructions"].isNull());
}

/* ======== Properties: deeply nested ======== */

void test_properties_deeply_nested(void) {
    Properties root;
    root.type = "object";

    Properties level1;
    level1.type = "object";
    Properties level2;
    level2.type = "array";
    level2.items.reset(new Properties());
    level2.items->type = "string";
    level1.properties["tags"] = level2;

    root.properties["config"] = level1;

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    root.toJson(obj);

    TEST_ASSERT_EQUAL_STRING("object",
        doc["properties"]["config"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("array",
        doc["properties"]["config"]["properties"]["tags"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string",
        doc["properties"]["config"]["properties"]["tags"]["items"]["type"].as<const char*>());
}

void test_properties_multiple_required(void) {
    Properties p;
    p.type = "object";
    p.required.push_back("a");
    p.required.push_back("b");
    p.required.push_back("c");

    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    p.toJson(obj);

    JsonArrayConst req = doc["required"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL(3, req.size());
    TEST_ASSERT_EQUAL_STRING("a", req[0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("b", req[1].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("c", req[2].as<const char*>());
}

/* ======== tools/list: complex schemas ======== */

void test_handle_tools_list_complex_schemas(void) {
    Tool tool;
    tool.name = "advanced";
    tool.description = "Advanced tool";
    tool.inputSchema.type = "object";
    tool.inputSchema.hasAdditionalProperties = true;
    tool.inputSchema.additionalProperties = false;

    Properties name;
    name.type = "string";
    name.description = "User name";
    name.format = "email";
    tool.inputSchema.properties["name"] = name;

    Properties tags;
    tags.type = "array";
    tags.items.reset(new Properties());
    tags.items->type = "string";
    tool.inputSchema.properties["tags"] = tags;

    Properties color;
    color.type = "string";
    color.enumValues.push_back("red");
    color.enumValues.push_back("blue");
    color.defaultValue = "red";
    tool.inputSchema.properties["color"] = color;

    tool.inputSchema.required.push_back("name");
    tool.handler = std::make_shared<EchoHandler>();
    server->RegisterTool(tool);

    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    MCPResponse res = server->handle(req);

    JsonVariantConst t = res.result()["tools"][0];
    JsonVariantConst schema = t["inputSchema"];
    TEST_ASSERT_FALSE(schema["additionalProperties"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("email", schema["properties"]["name"]["format"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string", schema["properties"]["tags"]["items"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL(2, schema["properties"]["color"]["enum"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("red", schema["properties"]["color"]["default"].as<const char*>());
    TEST_ASSERT_EQUAL(1, schema["required"].as<JsonArrayConst>().size());
}

/* ======== parseRequest: null id ======== */

void test_parse_null_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":null,"method":"initialize","params":{}})");
    TEST_ASSERT_EQUAL_STRING("initialize", req.method.c_str());
    TEST_ASSERT_TRUE(req.id().isNull());
}

void test_parse_zero_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":0,"method":"tools/list"})");
    TEST_ASSERT_EQUAL_STRING("tools/list", req.method.c_str());
    TEST_ASSERT_EQUAL(0, req.id().as<int>());
}

void test_parse_negative_id(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":-1,"method":"tools/list"})");
    TEST_ASSERT_EQUAL_STRING("tools/list", req.method.c_str());
    TEST_ASSERT_EQUAL(-1, req.id().as<int>());
}

/* ======== handle: ping ======== */

void test_handle_ping_returns_empty_result(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_TRUE(res.hasResult());
    TEST_ASSERT_FALSE(res.hasError());
    TEST_ASSERT_EQUAL(7, res.id().as<int>());

    /* The result must be an empty object, serialized as "result":{} */
    std::string json = server->serializeResponse(res);
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"result\":{}"));
}

void test_handle_ping_notification_gets_no_response(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","method":"ping"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_FALSE(res.hasBody());
    TEST_ASSERT_EQUAL_STRING("", server->serializeResponse(res).c_str());
}

/* ======== handle: protocol version 2025-06-18 ======== */

void test_handle_initialize_negotiates_2025_06_18(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_EQUAL(200, res.code);
    TEST_ASSERT_EQUAL_STRING("2025-06-18", res.result()["protocolVersion"].as<const char*>());
}

/* ======== handle: invalid request vs parse error ======== */

void test_valid_json_without_method_is_invalid_request(void) {
    /* Structurally valid JSON that is not a JSON-RPC request must yield
     * -32600 Invalid Request (with the id echoed), not -32700 Parse error. */
    MCPRequest req = server->parseRequest(R"({"jsonrpc":"2.0","id":3})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32600, res.error()["code"].as<int>());
    TEST_ASSERT_EQUAL(3, res.id().as<int>());
}

void test_empty_object_is_invalid_request_with_null_id(void) {
    MCPRequest req = server->parseRequest("{}");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32600, res.error()["code"].as<int>());
    TEST_ASSERT_TRUE(res.id().isNull());
}

/* ======== handle: notification semantics ======== */

class CountingHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        (void)params;
        calls++;
        JsonDocument result;
        result["ok"] = true;
        return result;
    }
    int calls = 0;
};

void test_notification_tools_call_gets_no_response_and_is_not_executed(void) {
    Tool tool;
    tool.name = "counter";
    tool.description = "Counts invocations";
    tool.inputSchema.type = "object";
    auto handler = std::make_shared<CountingHandler>();
    tool.handler = handler;
    server->RegisterTool(tool);

    /* No id → notification. JSON-RPC 2.0 forbids any response; MCP clients
     * never send tools/call as a notification, so it must not execute either. */
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"counter","arguments":{}}})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_FALSE(res.hasBody());
    TEST_ASSERT_EQUAL_STRING("", server->serializeResponse(res).c_str());
    TEST_ASSERT_EQUAL(0, handler->calls);
}

void test_notification_tools_list_gets_no_response(void) {
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","method":"tools/list"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_FALSE(res.hasBody());
}

void test_request_to_initialized_method_is_invalid_request(void) {
    /* notifications/initialized carrying an id is malformed — it must yield a
     * proper error object, not a body-less 202 or an empty response. */
    MCPRequest req = server->parseRequest(
        R"({"jsonrpc":"2.0","id":9,"method":"notifications/initialized"})");
    MCPResponse res = server->handle(req);

    TEST_ASSERT_TRUE(res.hasBody());
    TEST_ASSERT_TRUE(res.hasError());
    TEST_ASSERT_EQUAL(-32600, res.error()["code"].as<int>());
    TEST_ASSERT_EQUAL(9, res.id().as<int>());
}

/* ======== Main ======== */

int main(int argc, char** argv) {
    UNITY_BEGIN();

    /* parseRequest */
    RUN_TEST(test_parse_valid_request);
    RUN_TEST(test_parse_string_id);
    RUN_TEST(test_parse_invalid_json);
    RUN_TEST(test_parse_empty_string);
    RUN_TEST(test_parse_no_params);
    RUN_TEST(test_parse_with_arguments);

    /* handle: initialize */
    RUN_TEST(test_handle_initialize);
    RUN_TEST(test_handle_initialize_negotiates_supported_legacy_version);
    RUN_TEST(test_handle_initialize_falls_back_to_latest_for_unknown_version);
    RUN_TEST(test_handle_initialize_no_instructions);

    /* handle: notifications */
    RUN_TEST(test_handle_notifications_initialized);
    RUN_TEST(test_notification_tools_call_gets_no_response_and_is_not_executed);
    RUN_TEST(test_notification_tools_list_gets_no_response);
    RUN_TEST(test_request_to_initialized_method_is_invalid_request);

    /* handle: ping */
    RUN_TEST(test_handle_ping_returns_empty_result);
    RUN_TEST(test_handle_ping_notification_gets_no_response);

    /* handle: protocol versions */
    RUN_TEST(test_handle_initialize_negotiates_2025_06_18);

    /* handle: tools/list */
    RUN_TEST(test_handle_tools_list_empty);
    RUN_TEST(test_handle_tools_list_with_tool);
    RUN_TEST(test_handle_tools_list_multiple_tools);
    RUN_TEST(test_handle_tools_list_with_output_schema);

    /* handle: tools/call */
    RUN_TEST(test_handle_tool_call_success);
    RUN_TEST(test_handle_tool_call_handler_reports_execution_error);
    RUN_TEST(test_handle_tool_call_scalar_result_has_no_structured_content);
    RUN_TEST(test_handle_tool_call_missing_name);
    RUN_TEST(test_handle_tool_call_unknown_tool);
    RUN_TEST(test_handle_tool_call_null_handler);

    /* handle: errors */
    RUN_TEST(test_handle_unknown_method);
    RUN_TEST(test_handle_parse_error);
    RUN_TEST(test_valid_json_without_method_is_invalid_request);
    RUN_TEST(test_empty_object_is_invalid_request_with_null_id);

    /* serializeResponse */
    RUN_TEST(test_serialize_response_with_result);
    RUN_TEST(test_serialize_response_with_error);
    RUN_TEST(test_serialize_includes_jsonrpc_version);

    /* RegisterTool */
    RUN_TEST(test_register_tool);
    RUN_TEST(test_register_overwrites_same_name);

    /* Properties */
    RUN_TEST(test_properties_basic_type);
    RUN_TEST(test_properties_with_title_desc);
    RUN_TEST(test_properties_nested_with_required);
    RUN_TEST(test_properties_enum);
    RUN_TEST(test_properties_array_items);
    RUN_TEST(test_properties_additional_properties_false);
    RUN_TEST(test_properties_format);
    RUN_TEST(test_properties_default_value);
    RUN_TEST(test_properties_one_of);
    RUN_TEST(test_properties_any_of);
    RUN_TEST(test_properties_all_of);

    /* Tool::toString */
    RUN_TEST(test_tool_to_string);
    RUN_TEST(test_tool_to_string_with_output_schema);

    /* MCPResponse struct */
    RUN_TEST(test_response_default_constructor);
    RUN_TEST(test_response_id_constructor);
    RUN_TEST(test_response_code_id_constructor);

    /* MCPRequest struct */
    RUN_TEST(test_request_default_state);

    /* Properties deep copy */
    RUN_TEST(test_properties_copy_constructor_deep);
    RUN_TEST(test_properties_assignment_deep);
    RUN_TEST(test_properties_self_assignment);
    RUN_TEST(test_properties_copy_null_items);

    /* Properties::toString */
    RUN_TEST(test_properties_to_string);

    /* createJSONRPCError */
    RUN_TEST(test_create_error_all_codes);
    RUN_TEST(test_create_error_preserves_string_id);

    /* tools/call edge cases */
    RUN_TEST(test_handle_tool_call_empty_arguments);
    RUN_TEST(test_handle_tool_call_complex_result);
    RUN_TEST(test_handle_tool_call_empty_result);
    RUN_TEST(test_handle_tool_call_multiple_different_tools);

    /* Full roundtrip */
    RUN_TEST(test_roundtrip_initialize);
    RUN_TEST(test_roundtrip_tool_call);
    RUN_TEST(test_roundtrip_error);

    /* Default server */
    RUN_TEST(test_server_default_name_version);

    /* Properties: deeply nested */
    RUN_TEST(test_properties_deeply_nested);
    RUN_TEST(test_properties_multiple_required);

    /* tools/list: complex schemas */
    RUN_TEST(test_handle_tools_list_complex_schemas);

    /* parseRequest edge cases */
    RUN_TEST(test_parse_null_id);
    RUN_TEST(test_parse_zero_id);
    RUN_TEST(test_parse_negative_id);

    return UNITY_END();
}
