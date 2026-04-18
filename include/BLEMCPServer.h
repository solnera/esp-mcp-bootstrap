#ifndef BLE_MCP_SERVER_H
#define BLE_MCP_SERVER_H

#if __has_include(<NimBLEDevice.h>)

#include "MCPServer.h"
#include "McpBle.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class BLEMCPServer : public MCPServerBase {
   public:
    BLEMCPServer(const String& name = "ESP32-MCP-BLE",
                 const String& version = DEFAULT_SERVER_VERSION,
                 const String& instructions = "");
    ~BLEMCPServer();

    // Override BLE defaults (TX power, advertising, connection params, device name).
    // Must be called before begin() to take effect.
    void setBleConfig(const BleServerConfig& config);

    void begin();
    void end();
    void loop();

   private:
    static void onMessage(const char* message, void* ctx);
    void processMessage(const char* message);
    void sendResponse(const std::string& jsonResponse);

    static void taskEntry(void* ctx);
    static int sendBytes(const uint8_t* data, size_t len, void* ctx);
    static void onMtu(uint16_t mtu);
    static void sleepTicks(uint32_t ticks, void* ctx);
    static void logFn(int level, const char* tag, const char* message, void* ctx);

    QueueHandle_t rx_queue = nullptr;
    TaskHandle_t task_handle = nullptr;
    SemaphoreHandle_t task_done = nullptr;
    volatile bool exit_flag = false;

    static BLEMCPServer* s_bound;
    static bool s_initialized;
};

#endif  // __has_include(<NimBLEDevice.h>)
#endif  // BLE_MCP_SERVER_H
