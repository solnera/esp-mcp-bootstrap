#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <Arduino.h>
#include <ArduinoJson.h>

#include <map>
#include <memory>
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
    JsonDocument idDoc;
    JsonDocument paramsDoc;
    bool hasIdField;
    bool parseError;
    bool invalidRequest;

    MCPRequest() : method(""), hasIdField(false), parseError(false), invalidRequest(false) {}

    JsonVariantConst params() const {
        return paramsDoc.as<JsonVariantConst>();
    }

    JsonVariantConst id() const {
        return idDoc.as<JsonVariantConst>();
    }

    bool hasParams() const {
        return !paramsDoc.isNull();
    }

    bool isNotification() const {
        return !parseError && !hasIdField;
    }
};

struct MCPResponse {
    JsonDocument idDoc;
    JsonDocument resultDoc;
    JsonDocument errorDoc;
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
        return !resultDoc.isNull();
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

   protected:
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

    std::map<String, Tool> tools;
    String serverName;
    String serverVersion;
    String serverInstructions;
};

#endif  // MCP_SERVER_H
