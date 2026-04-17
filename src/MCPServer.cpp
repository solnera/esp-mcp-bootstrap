#include "MCPServer.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <cstring>

const char* const PROTOCOL_VERSION = "2025-11-25";
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
    obj["type"] = type;

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

    JsonObjectConst root = doc.as<JsonObjectConst>();
    request.method = root["method"].as<std::string>();
    request.hasIdField = !root["id"].isUnbound();
    request.idDoc.set(root["id"]);
    request.paramsDoc.set(root["params"]);
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
    if (request.method.empty()) {
        return createJSONRPCError(400, static_cast<int>(ErrorCode::PARSE_ERROR), request.id(), "Parse error: Invalid JSON");
    }

    if (request.method == "initialize") {
        return handleInitialize(request);
    } else if (request.method == "tools/list") {
        return handleToolsList(request);
    } else if (request.method == "notifications/initialized") {
        return handleInitialized(request);
    } else if (request.method == "tools/call") {
        return handleFunctionCalls(request);
    } else {
        if (request.isNotification()) {
            return MCPResponse(202, false);
        }
        return createJSONRPCError(200, static_cast<int>(ErrorCode::METHOD_NOT_FOUND), request.id(),
                                  "Method not found: " + request.method);
    }
}

MCPResponse MCPServerBase::handleInitialize(MCPRequest& request) {
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

MCPResponse MCPServerBase::handleInitialized(MCPRequest& request) {
    if (request.isNotification()) {
        return MCPResponse(202, false);
    }
    return MCPResponse(202, request.id());
}

MCPResponse MCPServerBase::handleToolsList(MCPRequest& request) {
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

    if (!params["name"].is<std::string>()) {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::INVALID_PARAMS), request.id(),
                                  "Missing or invalid 'name' parameter");
    }

    const char* functionName = params["name"].as<const char*>();
    JsonVariantConst arguments = params["arguments"];

    JsonObject result = mcpResponse.resultDoc.to<JsonObject>();
    JsonArray content = result["content"].to<JsonArray>();

    auto toolIt = tools.find(String(functionName));
    if (toolIt != tools.end()) {
        if (toolIt->second.handler) {
            JsonDocument resultDoc = toolIt->second.handler->call(arguments);

            String resultText;
            serializeJson(resultDoc, resultText);

            JsonObject textContent = content.add<JsonObject>();
            textContent["type"] = "text";
            textContent["text"] = resultText;
            result["structuredContent"].set(resultDoc.as<JsonVariantConst>());
            result["isError"] = false;
            return mcpResponse;
        } else {
            return createJSONRPCError(200, static_cast<int>(ErrorCode::INTERNAL_ERROR), request.id(),
                                      std::string("Tool handler not initialized: ") + functionName);
        }
    } else {
        return createJSONRPCError(200, static_cast<int>(ErrorCode::METHOD_NOT_FOUND), request.id(),
                                  std::string("Method not supported: ") + functionName);
    }
    return mcpResponse;
}

bool MCPServerBase::isSupportedProtocolVersion(const char* version) const {
    if (!version) {
        return false;
    }
    return strcmp(version, PROTOCOL_VERSION) == 0 ||
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
