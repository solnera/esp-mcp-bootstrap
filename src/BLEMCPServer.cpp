#if __has_include(<NimBLEDevice.h>)

#include "BLEMCPServer.h"
#include "McpBle.h"
#include "mcp_transport.h"
#include "esp_log.h"

#define TAG "MCP_SERVER"

BLEMCPServer* BLEMCPServer::s_bound = nullptr;
bool BLEMCPServer::s_initialized = false;

BLEMCPServer::BLEMCPServer(const String& name, const String& version, const String& instructions)
    : MCPServerBase(name, version, instructions) {
}

BLEMCPServer::~BLEMCPServer() {
    end();
}

void BLEMCPServer::setBleConfig(const BleServerConfig& config) {
    McpBle::getInstance().setConfig(config);
}

void BLEMCPServer::begin() {
    if (s_bound && s_bound != this) {
        Serial.println("[MCP_SERVER] Another BLE MCP server is already bound");
        return;
    }
    if (task_handle) {
        Serial.println("[MCP_SERVER] BLE MCP server is already running");
        return;
    }
    s_bound = this;

    exit_flag = false;

    if (!rx_queue) {
        rx_queue = xQueueCreate(4, sizeof(char*));
        if (!rx_queue) {
            Serial.println("[MCP_SERVER] Failed to create BLE RX queue");
            s_bound = nullptr;
            exit_flag = true;
            return;
        }
    }
    if (!task_done) {
        task_done = xSemaphoreCreateBinary();
        if (!task_done) {
            Serial.println("[MCP_SERVER] Failed to create BLE RX task completion semaphore");
            vQueueDelete(rx_queue);
            rx_queue = nullptr;
            s_bound = nullptr;
            exit_flag = true;
            return;
        }
    }
    if (!task_handle) {
        if (xTaskCreate(BLEMCPServer::taskEntry, "mcp_ble_rx", 8192, this, 1, &task_handle) != pdPASS) {
            Serial.println("[MCP_SERVER] Failed to create BLE RX task");
            vSemaphoreDelete(task_done);
            task_done = nullptr;
            vQueueDelete(rx_queue);
            rx_queue = nullptr;
            s_bound = nullptr;
            exit_flag = true;
            return;
        }
    }

    mcp_transport_set_sleep_fn(BLEMCPServer::sleepTicks, NULL);
    mcp_transport_set_log_fn(BLEMCPServer::logFn, NULL);

    if (!s_initialized) {
        mcp_transport_init();
        mcp_transport_set_send_fn(BLEMCPServer::sendBytes, NULL);
        mcp_transport_set_message_cb(BLEMCPServer::onMessage, this);
        mcp_transport_set_tx_gap_ticks(1);
        mcp_transport_set_send_retry(3, 1);

        McpBle::getInstance().setRxCallback([](const uint8_t* data, size_t len) {
            mcp_transport_receive(data, len);
        });

        McpBle::getInstance().setMtuCallback(BLEMCPServer::onMtu);
        mcp_transport_set_mtu(McpBle::getInstance().getMtu());

        McpBle::getInstance().init();

        vTaskDelay(pdMS_TO_TICKS(100)); // Brief yield for BLE stack init

        s_initialized = true;
    } else {
        mcp_transport_set_message_cb(BLEMCPServer::onMessage, this);
    }
}

void BLEMCPServer::end() {
    if (s_bound == this) {
        mcp_transport_set_message_cb(NULL, NULL);
    }

    if (task_handle) {
        exit_flag = true;
        char* sentinel = nullptr;
        if (rx_queue) {
            xQueueSend(rx_queue, &sentinel, 0);
        }
        if (task_done) {
            xSemaphoreTake(task_done, portMAX_DELAY);
        }
        task_handle = nullptr;
    }

    if (rx_queue) {
        char* msg = nullptr;
        while (xQueueReceive(rx_queue, &msg, 0) == pdTRUE) {
            if (msg) {
                free(msg);
                msg = nullptr;
            }
        }
        vQueueDelete(rx_queue);
        rx_queue = nullptr;
    }

    if (task_done) {
        vSemaphoreDelete(task_done);
        task_done = nullptr;
    }

    if (s_bound == this) {
        mcp_transport_deinit();
        s_bound = nullptr;
        s_initialized = false;
    }
}

void BLEMCPServer::loop() {
    if (!rx_queue) return;
    while (true) {
        char* msg = nullptr;
        if (xQueueReceive(rx_queue, &msg, 0) != pdTRUE) break;
        if (exit_flag) {
            if (msg) free(msg);
            break;
        }
        if (msg) {
            processMessage(msg);
            free(msg);
        }
    }
}

void BLEMCPServer::taskEntry(void* ctx) {
    auto* self = static_cast<BLEMCPServer*>(ctx);
    char* msg = nullptr;
    for (;;) {
        if (xQueueReceive(self->rx_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (self->exit_flag) {
                if (msg) free(msg);
                break;
            }
            if (msg) {
                self->processMessage(msg);
                free(msg);
                msg = nullptr;
            }
        }
    }
    if (self->task_done) {
        xSemaphoreGive(self->task_done);
    }
    vTaskDelete(NULL);
}

void BLEMCPServer::onMessage(const char* message, void* ctx) {
    if (!ctx) return;
    auto* self = static_cast<BLEMCPServer*>(ctx);
    if (self->exit_flag) return;
    if (!self->rx_queue || !message) return;
    size_t n = strlen(message);
    char* copy = (char*)malloc(n + 1);
    if (!copy) return;
    memcpy(copy, message, n);
    copy[n] = '\0';
    if (xQueueSend(self->rx_queue, &copy, 0) != pdTRUE) {
        free(copy);
    }
}

int BLEMCPServer::sendBytes(const uint8_t* data, size_t len, void* ctx) {
    (void)ctx;
    return McpBle::getInstance().sendNotification(data, len) ? 0 : -1;
}

void BLEMCPServer::onMtu(uint16_t mtu) {
    mcp_transport_set_mtu(mtu);
}

void BLEMCPServer::sleepTicks(uint32_t ticks, void* ctx) {
    (void)ctx;
    if (ticks > 0) vTaskDelay(ticks);
}

void BLEMCPServer::logFn(int level, const char* tag, const char* message, void* ctx) {
    (void)ctx;
    if (!tag || !message) return;
    Serial.printf("[%s] %s\n", tag, message);
}

void BLEMCPServer::processMessage(const char* message) {
    MCPRequest request = parseRequest(message);
    MCPResponse response = handle(request);
    std::string jsonResponse = serializeResponse(response);
    if (!response.hasBody() || jsonResponse.empty()) {
        return;
    }
    sendResponse(jsonResponse);
}

void BLEMCPServer::sendResponse(const std::string& jsonResponse) {
    mcp_transport_send_message(jsonResponse.c_str());
}

#endif  // __has_include(<NimBLEDevice.h>)
