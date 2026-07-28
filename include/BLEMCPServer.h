#ifndef BLE_MCP_SERVER_H
#define BLE_MCP_SERVER_H

#if __has_include(<NimBLEDevice.h>)

#include "MCPServer.h"
#include "McpBle.h"
#include "mcp_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>

// Depth of the RX hand-off queue between the BLE write callback and the worker
// task. A full queue means requests are arriving faster than they are handled;
// overflow is counted (see droppedMessageCount) rather than dropped silently.
#ifndef MCP_BLE_RX_QUEUE_DEPTH
#define MCP_BLE_RX_QUEUE_DEPTH 16
#endif

// Aggregate heap budget for complete messages waiting in the BLE worker queue.
// This is independent of queue depth so a burst of maximum-size messages cannot
// consume most of an ESP32's heap. Small messages can still use the full queue.
#ifndef MCP_BLE_RX_QUEUE_MAX_BYTES
#define MCP_BLE_RX_QUEUE_MAX_BYTES (MCP_TRANSPORT_MAX_MESSAGE_SIZE * 2)
#endif

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

    // Number of inbound messages dropped because the RX queue was full.
    // Non-zero means the peer is sending faster than handlers can keep up.
    uint32_t droppedMessageCount() const { return rx_dropped.load(std::memory_order_relaxed); }
    size_t queuedMessageBytes() const { return rx_queued_bytes.load(std::memory_order_relaxed); }

   private:
    static void onMessage(const char* message, void* ctx);
    void processMessage(const char* message);
    void sendResponse(const std::string& jsonResponse);

    static void taskEntry(void* ctx);
    static int sendBytes(const uint8_t* data, size_t len, void* ctx);
    static void onMtu(uint16_t mtu);
    static void sleepTicks(uint32_t ticks, void* ctx);
    static void logFn(int level, const char* tag, const char* message, void* ctx);
    static void lockFn(bool lock, void* ctx);

    QueueHandle_t rx_queue = nullptr;
    TaskHandle_t task_handle = nullptr;
    SemaphoreHandle_t task_done = nullptr;
    SemaphoreHandle_t send_mutex = nullptr;
    std::atomic<bool> exit_flag{false};
    std::atomic<uint32_t> rx_dropped{0};
    std::atomic<size_t> rx_queued_bytes{0};

    static BLEMCPServer* s_bound;
    static bool s_initialized;
};

#endif  // __has_include(<NimBLEDevice.h>)
#endif  // BLE_MCP_SERVER_H
