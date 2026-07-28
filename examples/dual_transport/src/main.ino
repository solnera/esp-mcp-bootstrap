#include <Arduino.h>
#include <WiFi.h>
#include <BLEMCPServer.h>
#include <HttpMCPServer.h>

const uint16_t MCP_PORT = 3000;

BLEMCPServer bleServer("ESP32-MCP-BLE", "1.0.0", "Dual transport MCP server");
HttpMCPServer* httpServer = nullptr;
Tool echoTool;

static String ipToString(const IPAddress& ip) {
    return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

static String httpUrl() {
    if (httpServer == nullptr || WiFi.status() != WL_CONNECTED) {
        return String("");
    }
    return String("http://") + ipToString(WiFi.localIP()) + ":" + String(MCP_PORT) + "/mcp";
}

class EchoHandler : public ToolHandler {
   public:
    JsonDocument call(JsonVariantConst params) override {
        JsonDocument result;
        String text = params["text"].as<String>();
        Serial.printf("[Echo Tool] Received text: %s\n", text.c_str());
        result["echo"] = text;
        result["length"] = text.length();
        result["timestamp"] = millis();
        return result;
    }
};

class ConfigWifiHandler : public ToolHandler {
   public:
    JsonDocument call(JsonVariantConst params) override {
        const char* ssid = params["ssid"].isNull() ? "" : params["ssid"].as<const char*>();
        const char* password = params["password"].isNull() ? "" : params["password"].as<const char*>();
        Serial.printf("WiFi config: ssid=%s\n", ssid);

        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(200);
        WiFi.begin(ssid, password);

        const unsigned long startMs = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 20000) {
            delay(250);
        }

        JsonDocument result;
        if (WiFi.status() == WL_CONNECTED) {
            String ipStr = ipToString(WiFi.localIP());
            Serial.printf("WiFi connected: ssid=%s, ip=%s\n", WiFi.SSID().c_str(), ipStr.c_str());

            if (httpServer == nullptr) {
                httpServer = new HttpMCPServer(MCP_PORT, "ESP32-MCP-HTTP", "1.0.0",
                                               "Dual transport MCP server");
                httpServer->RegisterTool(echoTool);
                httpServer->begin();
                Serial.printf("HTTP MCP Server started at http://%s:%d/mcp\n",
                              ipStr.c_str(), MCP_PORT);
            }

            result["status"] = "connected";
            result["ssid"] = WiFi.SSID();
            result["ip"] = ipStr;
            result["http_url"] = httpUrl();
        } else {
            result["status"] = "failed";
            result["ssid"] = WiFi.SSID();
            result["ip"] = "";
            result["http_url"] = "";
        }
        return result;
    }
};

class GetStatusHandler : public ToolHandler {
   public:
    JsonDocument call(JsonVariantConst params) override {
        (void)params;
        JsonDocument result;
        const bool connected = WiFi.status() == WL_CONNECTED;
        result["status"] = connected ? "connected" : "disconnected";
        result["ssid"] = WiFi.SSID();
        result["ip"] = connected ? ipToString(WiFi.localIP()) : String("");
        result["http_url"] = httpUrl();
        return result;
    }
};

Tool createEchoTool() {
    Tool tool;
    tool.name = "echo";
    tool.description = "Echo back the input text";
    tool.inputSchema.type = "object";
    Properties textProperty;
    textProperty.type = "string";
    textProperty.description = "Text to echo back";
    tool.inputSchema.properties["text"] = textProperty;
    tool.inputSchema.required.push_back("text");
    tool.handler = std::make_shared<EchoHandler>();
    return tool;
}

Tool createConfigWifiTool() {
    Tool tool;
    tool.name = "config_wifi";
    tool.description = "Configure WiFi with ssid and password; starts HTTP MCP transport on success";
    tool.inputSchema.type = "object";

    Properties ssidProp;
    ssidProp.type = "string";
    ssidProp.description = "WiFi SSID";
    tool.inputSchema.properties["ssid"] = ssidProp;
    tool.inputSchema.required.push_back("ssid");

    Properties passwordProp;
    passwordProp.type = "string";
    passwordProp.description = "WiFi password";
    tool.inputSchema.properties["password"] = passwordProp;
    tool.inputSchema.required.push_back("password");

    tool.handler = std::make_shared<ConfigWifiHandler>();
    return tool;
}

Tool createGetStatusTool() {
    Tool tool;
    tool.name = "get_status";
    tool.description = "Get current WiFi status and HTTP MCP URL";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<GetStatusHandler>();
    return tool;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n========================================");
    Serial.println("ESP32 MCP Server - Dual Transport");
    Serial.println("========================================");

    echoTool = createEchoTool();

    bleServer.RegisterTool(echoTool);
    bleServer.RegisterTool(createConfigWifiTool());
    bleServer.RegisterTool(createGetStatusTool());
    bleServer.begin();
    Serial.println("BLE MCP Server started");
    Serial.println("Waiting for BLE config_wifi to bring up HTTP transport...");
}

void loop() {
    delay(1000);
}
