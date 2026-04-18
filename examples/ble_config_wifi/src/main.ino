#include <Arduino.h>
#include <WiFi.h>
#include <BLEMCPServer.h>

BLEMCPServer mcpServer("ESP32-MCP-BLE", "1.0.0", "MCP WiFi configuration tool");

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
        result["status"] = (WiFi.status() == WL_CONNECTED) ? "connected" : "failed";
        result["ssid"] = WiFi.SSID();
        if (WiFi.status() == WL_CONNECTED) {
            IPAddress ip = WiFi.localIP();
            String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
            Serial.printf("WiFi connected: ssid=%s, ip=%s\n", WiFi.SSID().c_str(), ipStr.c_str());
            result["ip"] = ipStr;
        } else {
            result["ip"] = "";
        }
        return result;
    }
};

class GetStatusHandler : public ToolHandler {
   public:
    JsonDocument call(JsonVariantConst params) override {
        (void)params;
        JsonDocument result;
        result["status"] = (WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected";
        result["ssid"] = WiFi.SSID();
        if (WiFi.status() == WL_CONNECTED) {
            IPAddress ip = WiFi.localIP();
            String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
            result["ip"] = ipStr;
        } else {
            result["ip"] = "";
        }
        return result;
    }
};

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== ESP32 MCP Server Starting (BLE) ===");

    Serial.println("Registering tools...");
    Tool configWifiTool;
    configWifiTool.name = "config_wifi";
    configWifiTool.description = "Configure WiFi with ssid and password";
    configWifiTool.inputSchema.type = "object";

    Properties ssidProp;
    ssidProp.type = "string";
    ssidProp.description = "WiFi SSID";
    configWifiTool.inputSchema.properties["ssid"] = ssidProp;
    configWifiTool.inputSchema.required.push_back("ssid");

    Properties passwordProp;
    passwordProp.type = "string";
    passwordProp.description = "WiFi password";
    configWifiTool.inputSchema.properties["password"] = passwordProp;
    configWifiTool.inputSchema.required.push_back("password");
    configWifiTool.handler = std::make_shared<ConfigWifiHandler>();
    mcpServer.RegisterTool(configWifiTool);

    Tool getStatusTool;
    getStatusTool.name = "get_status";
    getStatusTool.description = "Get current WiFi status";
    getStatusTool.inputSchema.type = "object";
    getStatusTool.handler = std::make_shared<GetStatusHandler>();
    mcpServer.RegisterTool(getStatusTool);

    Serial.println("Starting MCP server...");
    BleServerConfig bleCfg;
    bleCfg.txPower = ESP_PWR_LVL_P9;        // +9 dBm, max range
    bleCfg.advTxPower = ESP_PWR_LVL_P9;
    bleCfg.advMinInterval = 0x20;           // 20 ms, fast discovery
    bleCfg.advMaxInterval = 0x30;           // 30 ms
    bleCfg.connMinInterval = 6;             // 7.5 ms, high throughput
    bleCfg.connMaxInterval = 12;            // 15 ms
    bleCfg.connSlaveLatency = 0;            // no skipped events
    bleCfg.connSupervisionTimeout = 200;    // 2 s
    mcpServer.setBleConfig(bleCfg);
    mcpServer.begin();

    Serial.printf("\n=== MCP Server Ready ===\n");
    Serial.printf("Waiting for BLE connections to configure WiFi...\n\n");
}

void loop() {
    delay(1000);
}
