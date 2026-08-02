#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// MCP protocol versions supported by this library. The default is the latest
// version used when the client does not request a supported legacy version.
extern const char* const PROTOCOL_VERSION;
extern const char* const PROTOCOL_VERSION_2025_06_18;
extern const char* const PROTOCOL_VERSION_2025_03_26;
extern const char* const PROTOCOL_VERSION_2024_11_05;
extern const char* const DEFAULT_SERVER_NAME;
extern const char* const DEFAULT_SERVER_VERSION;

struct MCPRequest {
    std::string method;

    /* The parse result is retained whole and params() is a view into it, so a
     * tools/call payload is never deep-copied out into a second document. The
     * id keeps its own (scalar-sized) copy: parseRequest deliberately leaves it
     * null for a malformed id so the reply carries id:null, which a view into
     * doc could not express. */
    JsonDocument doc;
    JsonDocument idDoc;
    bool hasIdField;
    bool parseError;
    bool invalidRequest;
    // Set only once params has passed validation, so the early-return paths
    // expose no params at all — as they did when params had its own document.
    bool paramsChecked;

    MCPRequest()
        : method(""), hasIdField(false), parseError(false), invalidRequest(false), paramsChecked(false) {}

    JsonVariantConst params() const {
        return paramsChecked ? doc["params"] : JsonVariantConst();
    }

    JsonVariantConst id() const {
        return idDoc.as<JsonVariantConst>();
    }

    bool hasParams() const {
        return paramsChecked && !doc["params"].isNull();
    }

    bool isNotification() const {
        return !parseError && !hasIdField;
    }
};

struct MCPResponse {
    JsonDocument idDoc;
    JsonDocument resultDoc;
    JsonDocument errorDoc;

    /* Already-serialized result JSON, spliced straight into the reply. Lets a
     * handler hand back a cached body (tools/list) without rebuilding the tree
     * and without a document-to-document deep copy. Takes precedence over
     * resultDoc when non-empty. */
    std::string rawResult;

    int code;
    bool body;

    MCPResponse() : code(200), body(true) {}
    MCPResponse(const JsonVariantConst& id) : code(200), body(true) {
        idDoc.set(id);
    }
    MCPResponse(int code, const JsonVariantConst& id) : code(code), body(true) {
        idDoc.set(id);
    }
    MCPResponse(int code, bool body) : code(code), body(body) {}

    JsonVariantConst id() const {
        return idDoc.as<JsonVariantConst>();
    }
    JsonVariantConst result() const {
        return resultDoc.as<JsonVariantConst>();
    }
    JsonVariantConst error() const {
        return errorDoc.as<JsonVariantConst>();
    }

    bool hasResult() const {
        return !rawResult.empty() || !resultDoc.isNull();
    }
    bool hasError() const {
        return !errorDoc.isNull();
    }
    bool hasBody() const {
        return body;
    }
};

// JSON-RPC error codes
enum class ErrorCode {
    SERVER_ERROR = -32000,
    INVALID_REQUEST = -32600,
    METHOD_NOT_FOUND = -32601,
    INVALID_PARAMS = -32602,
    INTERNAL_ERROR = -32603,
    PARSE_ERROR = -32700
};

class ToolHandler {
   public:
    virtual ~ToolHandler() = default;
    virtual JsonDocument call(JsonVariantConst params) = 0;

    /* Error-reporting variant; this is what the dispatcher invokes. The default
     * runs call() and reports success, so existing single-method handlers keep
     * working unchanged. To signal an execution failure (surfaced to the client
     * as result.isError = true, per MCP), override BOTH overloads and set
     * isError here; call(params) can simply delegate:
     *   bool ignored; return call(params, ignored); */
    virtual JsonDocument call(JsonVariantConst params, bool& isError) {
        isError = false;
        return call(params);
    }
};

class Properties {
   public:
    Properties() = default;

    Properties(const Properties& other) {
        type = other.type;
        title = other.title;
        description = other.description;
        properties = other.properties;
        required = other.required;
        additionalProperties = other.additionalProperties;
        hasAdditionalProperties = other.hasAdditionalProperties;
        if (other.items) {
            items.reset(new Properties(*other.items));
        } else {
            items.reset(nullptr);
        }
        enumValues = other.enumValues;
        oneOf = other.oneOf;
        anyOf = other.anyOf;
        allOf = other.allOf;
        format = other.format;
        defaultValue = other.defaultValue;
    }

    Properties& operator=(const Properties& other) {
        if (this != &other) {
            type = other.type;
            title = other.title;
            description = other.description;
            properties = other.properties;
            required = other.required;
            additionalProperties = other.additionalProperties;
            hasAdditionalProperties = other.hasAdditionalProperties;
            if (other.items) {
                items.reset(new Properties(*other.items));
            } else {
                items.reset(nullptr);
            }
            enumValues = other.enumValues;
            oneOf = other.oneOf;
            anyOf = other.anyOf;
            allOf = other.allOf;
            format = other.format;
            defaultValue = other.defaultValue;
        }
        return *this;
    }

    Properties(Properties&&) = default;
    Properties& operator=(Properties&&) = default;

    String type;
    String title;
    String description;
    std::map<String, Properties> properties;
    std::vector<String> required;

    bool additionalProperties = true;
    bool hasAdditionalProperties = false;

    std::unique_ptr<Properties> items;

    std::vector<String> enumValues;

    std::vector<Properties> oneOf;
    std::vector<Properties> anyOf;
    std::vector<Properties> allOf;

    String format;

    String defaultValue;

    String toString() const;
    void toJson(JsonObject& obj) const;
};

class Tool {
   public:
    Tool() = default;
    Tool(const Tool&) = default;
    Tool& operator=(const Tool&) = default;
    Tool(Tool&&) = default;
    Tool& operator=(Tool&&) = default;

    String name;
    String description;
    Properties inputSchema;
    Properties outputSchema;
    std::shared_ptr<ToolHandler> handler;

    String toString() const;
};

// Base class for MCP server implementations
class MCPServerBase {
   public:
    MCPServerBase(const String& name = DEFAULT_SERVER_NAME,
                  const String& version = DEFAULT_SERVER_VERSION,
                  const String& instructions = "");
    virtual ~MCPServerBase() = default;

    void RegisterTool(const Tool& tool);
    // Overload for callers that can give up ownership: skips the deep copy of
    // the (recursive) schema tree. Equivalent in every other respect.
    void RegisterTool(Tool&& tool);

   protected:
    /* The const char* form is the real one; both transports hand us a
     * NUL-terminated buffer, and routing that through std::string built a
     * throwaway copy of the whole request body on every call. */
    MCPRequest parseRequest(const char* json);
    MCPRequest parseRequest(const std::string& json);
    std::string serializeResponse(const MCPResponse& response);
    MCPResponse createJSONRPCError(int httpCode, int rpcCode, const JsonVariantConst& id, const std::string& message);
    MCPResponse handle(MCPRequest& request);
    MCPResponse handleInitialize(MCPRequest& request);
    MCPResponse handlePing(MCPRequest& request);
    MCPResponse handleToolsList(MCPRequest& request);
    MCPResponse handleFunctionCalls(MCPRequest& request);
    bool isSupportedProtocolVersion(const char* version) const;
    const char* negotiateProtocolVersion(JsonVariantConst params) const;

    /* Serialized tools/list body, rebuilt only after the tool set changes.
     * Capabilities advertise listChanged:false, so between registrations this
     * is a constant — and clients ask for it on every session start.
     *
     * INVARIANT: unsynchronized. Unlike the rest of handle(), this touches
     * mutable server state, so it may only be reached from the one task that
     * drives a given server instance — async_tcp for HTTP (the worker queue
     * takes tools/call and nothing else), the RX worker for BLE. Routing
     * tools/list onto a second task would need a lock here. */
    const std::string& toolsListJson();

    /* Keyed on std::string rather than Arduino String so a lookup key built
     * from the request costs no heap: short-string optimization keeps tool
     * names of ~15 characters entirely on the stack, whereas String always
     * allocates. Deliberately NOT std::less<> — a transparent comparator would
     * drop the temporary entirely, but it is C++14 and this is a public header
     * compiled as part of the consumer's sketch, which the ESP32 Arduino
     * framework still builds as gnu++11. */
    std::map<std::string, Tool> tools;
    std::string toolsListCache;
    bool toolsListDirty = true;

    String serverName;
    String serverVersion;
    String serverInstructions;
};

#endif  // MCP_SERVER_H
