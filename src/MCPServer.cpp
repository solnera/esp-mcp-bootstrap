#include "MCPServer.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <cstring>

const char* const PROTOCOL_VERSION = "2025-11-25";
const char* const PROTOCOL_VERSION_2025_06_18 = "2025-06-18";
const char* const PROTOCOL_VERSION_2025_03_26 = "2025-03-26";
const char* const PROTOCOL_VERSION_2024_11_05 = "2024-11-05";
const char* const DEFAULT_SERVER_NAME = "ESP32-MCP-Server";
const char* const DEFAULT_SERVER_VERSION = "1.0.0";

String Properties::toString() const {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    toJson(obj);
    String result;
    serializeJson(doc, result);
    return result;
}

void Properties::toJson(JsonObject& obj) const {
    // An omitted type is a valid unconstrained JSON Schema. Emitting an empty
    // string produces an invalid schema and breaks tools that forgot to set it.
    if (type.length() > 0) {
        obj["type"] = type;
    }

    if (title.length() > 0) {
        obj["title"] = title;
    }

    if (description.length() > 0) {
        obj["description"] = description;
    }

    if (!properties.empty()) {
        JsonObject propertiesObj = obj["properties"].to<JsonObject>();
        for (const auto& kv : properties) {
            const String& key = kv.first;
            const Properties& value = kv.second;
            JsonObject propObj = propertiesObj[key].to<JsonObject>();
            value.toJson(propObj);
        }
    }

    if (!required.empty()) {
        JsonArray requiredArray = obj["required"].to<JsonArray>();
        for (const auto& req : required) {
            requiredArray.add(req);
        }
    }

    if (hasAdditionalProperties) {
        obj["additionalProperties"] = additionalProperties;
    }

    if (items) {
        JsonObject itemsObj = obj["items"].to<JsonObject>();
        items->toJson(itemsObj);
    }

    if (!enumValues.empty()) {
        JsonArray enumArray = obj["enum"].to<JsonArray>();
        for (const auto& value : enumValues) {
            enumArray.add(value);
        }
    }

    if (!oneOf.empty()) {
        JsonArray oneOfArray = obj["oneOf"].to<JsonArray>();
        for (const auto& schema : oneOf) {
            JsonObject schemaObj = oneOfArray.add<JsonObject>();
            schema.toJson(schemaObj);
        }
    }

    if (!anyOf.empty()) {
        JsonArray anyOfArray = obj["anyOf"].to<JsonArray>();
        for (const auto& schema : anyOf) {
            JsonObject schemaObj = anyOfArray.add<JsonObject>();
            schema.toJson(schemaObj);
        }
    }

    if (!allOf.empty()) {
        JsonArray allOfArray = obj["allOf"].to<JsonArray>();
        for (const auto& schema : allOf) {
            JsonObject schemaObj = allOfArray.add<JsonObject>();
            schema.toJson(schemaObj);
        }
    }

    if (format.length() > 0) {
        obj["format"] = format;
    }

    if (defaultValue.length() > 0) {
        obj["default"] = defaultValue;
    }
}

String Tool::toString() const {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();

    obj["name"] = name;
    obj["description"] = description;

    JsonObject inputSchemaObj = obj["inputSchema"].to<JsonObject>();
    inputSchema.toJson(inputSchemaObj);

    if (outputSchema.type.length() > 0) {
        JsonObject outputSchemaObj = obj["outputSchema"].to<JsonObject>();
        outputSchema.toJson(outputSchemaObj);
    }

    String result;
    serializeJson(doc, result);
    return result;
}

MCPServerBase::MCPServerBase(const String& name, const String& version, const String& instructions)
    : serverName(name), serverVersion(version), serverInstructions(instructions) {
}

void MCPServerBase::RegisterTool(const Tool& tool) {
    tools[tool.name] = tool;
}

MCPRequest MCPServerBase::parseRequest(const std::string& json) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);

    MCPRequest request;

    if (error) {
        request.method = "";
        request.parseError = true;
        return request;
    }

    if (!doc.is<JsonObjectConst>()) {
        request.invalidRequest = true;
        return request;
    }

    JsonObjectConst root = doc.as<JsonObjectConst>();
    JsonVariantConst versionVar = root["jsonrpc"];
    JsonVariantConst methodVar = root["method"];
    JsonVariantConst idVar = root["id"];
    JsonVariantConst paramsVar = root["params"];

    request.hasIdField = !idVar.isUnbound();
    if (request.hasIdField &&
        !(idVar.isNull() || idVar.is<const char*>() || idVar.is<int64_t>() || idVar.is<uint64_t>())) {
        request.invalidRequest = true;
        request.hasIdField = false;  // invalid ids must be answered as id:null
        return request;
    }
    request.idDoc.set(idVar);

    if (!versionVar.is<const char*>() || strcmp(versionVar.as<const char*>(), "2.0") != 0 ||
        !methodVar.is<const char*>()) {
        request.invalidRequest = true;
        return request;
    }

    if (!paramsVar.isUnbound() && !(paramsVar.is<JsonObjectConst>() || paramsVar.is<JsonArrayConst>())) {
        request.invalidRequest = true;
        return request;
    }

    request.method = methodVar.as<const char*>();
    request.paramsDoc.set(paramsVar);
    return request;
}

std::string MCPServerBase::serializeResponse(const MCPResponse& response) {
    if (!response.hasBody()) {
        return "";
    }

    JsonDocument doc;
    doc["id"] = response.id();
    doc["jsonrpc"] = "2.0";

    if (response.hasResult()) {
        doc["result"] = response.result();
    }
    if (response.hasError()) {
        doc["error"] = response.error();
    }

    std::string jsonResponse;
    serializeJson(doc, jsonResponse);
    return jsonResponse;
}

MCPResponse MCPServerBase::handle(MCPRequest& request) {
    if (request.parseError) {
        return createJSONRPCError(400, static_cast<int>(ErrorCode::PARSE_ERROR), request.id(), "Parse error: Invalid JSON");
    }

    if (request.invalidRequest || request.method.empty()) {
        return createJSONRPCError(400, static_cast<int>(ErrorCode::INVALID_REQUEST), request.id(),
                                  "Invalid Request");
    }

    if (request.isNotification()) {
        // JSON-RPC 2.0: a notification never receives a response, whether the
        // method is known or not. notifications/initialized carries no state we
        // need to track, so acknowledging at the transport level is all that's
        // required (HTTP maps this to 202 Accepted with no body; BLE sends nothing).
        return MCPResponse(202, false);
    }

    if (request.method == "initialize") {
        return handleInitialize(request);
    } else if (request.method == "ping") {
        return handlePing(request);
    } else if (request.method == "tools/list") {
        return handleToolsList(request);
    } else if (request.method == "tools/call") {
        return handleFunctionCalls(request);
    } else if (request.method == "notifications/initialized") {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_REQUEST), request.id(),
                                  "notifications/initialized must be sent as a notification");
    } else {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::METHOD_NOT_FOUND), request.id(),
                                  "Method not found: " + request.method);
    }
}

MCPResponse MCPServerBase::handleInitialize(MCPRequest& request) {
    JsonVariantConst params = request.params();
    if (!params.is<JsonObjectConst>() || !params["protocolVersion"].is<const char*>() ||
        !params["capabilities"].is<JsonObjectConst>() || !params["clientInfo"].is<JsonObjectConst>() ||
        !params["clientInfo"]["name"].is<const char*>() ||
        !params["clientInfo"]["version"].is<const char*>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "Invalid initialize parameters");
    }

    MCPResponse response(200, request.id());
    JsonObject result = response.resultDoc.to<JsonObject>();

    result["protocolVersion"] = negotiateProtocolVersion(request.params());

    JsonObject capabilities = result["capabilities"].to<JsonObject>();
    JsonObject toolsCap = capabilities["tools"].to<JsonObject>();
    toolsCap["listChanged"] = false;

    JsonObject serverInfo = result["serverInfo"].to<JsonObject>();
    serverInfo["name"] = serverName;
    serverInfo["version"] = serverVersion;

    if (serverInstructions.length() > 0) {
        result["instructions"] = serverInstructions;
    }
    return response;
}

MCPResponse MCPServerBase::handlePing(MCPRequest& request) {
    MCPResponse response(200, request.id());
    // The spec requires a prompt empty-result response; clients use ping to
    // probe liveness and may drop the connection on a -32601.
    response.resultDoc.to<JsonObject>();
    return response;
}

MCPResponse MCPServerBase::handleToolsList(MCPRequest& request) {
    if (request.hasParams() && !request.params().is<JsonObjectConst>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "tools/list params must be an object");
    }

    MCPResponse response(200, request.id());
    JsonObject result = response.resultDoc.to<JsonObject>();
    JsonArray toolsArray = result["tools"].to<JsonArray>();
    for (const auto& [key, value] : tools) {
        JsonObject tool = toolsArray.add<JsonObject>();
        tool["name"] = key;
        tool["description"] = value.description;

        JsonObject inputSchemaObj = tool["inputSchema"].to<JsonObject>();
        value.inputSchema.toJson(inputSchemaObj);

        if (value.outputSchema.type.length() > 0) {
            JsonObject outputSchemaObj = tool["outputSchema"].to<JsonObject>();
            value.outputSchema.toJson(outputSchemaObj);
        }
    }
    return response;
}

MCPResponse MCPServerBase::handleFunctionCalls(MCPRequest& request) {
    MCPResponse mcpResponse(200, request.id());
    JsonVariantConst params = request.params();

    if (!params.is<JsonObjectConst>() || !params["name"].is<const char*>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "Missing or invalid 'name' parameter");
    }

    const char* functionName = params["name"].as<const char*>();
    JsonVariantConst arguments = params["arguments"];
    if (!arguments.isUnbound() && !arguments.is<JsonObjectConst>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "'arguments' must be an object");
    }

    JsonObject result = mcpResponse.resultDoc.to<JsonObject>();
    JsonArray content = result["content"].to<JsonArray>();

    auto toolIt = tools.find(String(functionName));
    if (toolIt != tools.end()) {
        if (toolIt->second.handler) {
            bool toolError = false;
            JsonDocument resultDoc = toolIt->second.handler->call(arguments, toolError);

            String resultText;
            serializeJson(resultDoc, resultText);

            JsonObject textContent = content.add<JsonObject>();
            textContent["type"] = "text";
            textContent["text"] = resultText;
            if (!toolError && resultDoc.is<JsonObjectConst>()) {
                /* structuredContent is defined by MCP as a JSON object, and an
                 * error payload would not conform to a declared outputSchema —
                 * attach it only for successful object results. Non-object
                 * results still reach the client as serialized text content. */
                result["structuredContent"].set(resultDoc.as<JsonVariantConst>());
            }
            result["isError"] = toolError;
            return mcpResponse;
        } else {
            return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR), request.id(),
                                      std::string("Tool handler not initialized: ") + functionName);
        }
    } else {
        /* Per MCP, an unknown tool is a -32602 invalid-params protocol error
         * ("Unknown tool: ..."), not -32601 — the method (tools/call) exists. */
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  std::string("Unknown tool: ") + functionName);
    }
    return mcpResponse;
}

bool MCPServerBase::isSupportedProtocolVersion(const char* version) const {
    if (!version) {
        return false;
    }
    return strcmp(version, PROTOCOL_VERSION) == 0 ||
           strcmp(version, PROTOCOL_VERSION_2025_06_18) == 0 ||
           strcmp(version, PROTOCOL_VERSION_2025_03_26) == 0 ||
           strcmp(version, PROTOCOL_VERSION_2024_11_05) == 0;
}

const char* MCPServerBase::negotiateProtocolVersion(JsonVariantConst params) const {
    const char* requested = params["protocolVersion"].as<const char*>();
    if (isSupportedProtocolVersion(requested)) {
        return requested;
    }
    return PROTOCOL_VERSION;
}

MCPResponse MCPServerBase::createJSONRPCError(int httpCode, int rpcCode, const JsonVariantConst& id,
                                              const std::string& message) {
    MCPResponse response(httpCode, id);

    response.errorDoc["code"] = rpcCode;
    response.errorDoc["message"] = message;

    return response;
}
