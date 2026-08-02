#pragma once

#if __has_include(<NimBLEDevice.h>)

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <functional>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct BleServerConfig {
    // Advertised device name.
    std::string deviceName = "MCP_Server_BLE";

    // TX power. txPower applies to default/connection path, advTxPower to advertising.
    // +3 dBm trades ~40% current vs +9 dBm while still reaching typical phone distances.
    esp_power_level_t txPower = ESP_PWR_LVL_P3;
    esp_power_level_t advTxPower = ESP_PWR_LVL_P3;

    // Advertising interval, units of 0.625 ms.
    uint16_t advMinInterval = 0x320;  // 500 ms
    uint16_t advMaxInterval = 0x640;  // 1000 ms
    bool scanResponse = true;

    // Connection parameter update requested on link-up.
    // Interval units: 1.25 ms. Supervision timeout units: 10 ms.
    uint16_t connMinInterval = 12;          // 15 ms
    uint16_t connMaxInterval = 24;          // 30 ms
    uint16_t connSlaveLatency = 4;
    uint16_t connSupervisionTimeout = 400;  // 4 s

    /* Preferred ATT MTU offered during negotiation. Fragment count is set by
     * the negotiated MTU: at the 23-byte default each packet carries 19 payload
     * bytes, at 517 it carries 511 — a ~27x difference in packets for the same
     * message. A central that cannot go this high simply negotiates down, so
     * this is safe to leave at the maximum. */
    uint16_t preferredMtu = 517;

    /* Delay inserted between outbound fragments, in ticks. Was unconditionally
     * 1 tick, which on a 1 kHz tick meant an 8 KiB message at the default MTU
     * spent ~430 ms asleep. Backpressure is already handled by the send-retry
     * path, so the default is now 0; raise it if a particular central drops
     * notifications under back-to-back writes. */
    uint32_t txGapTicks = 0;
};

class McpBle {
public:
    using RxCallback = std::function<void(const uint8_t* data, size_t len)>;
    using MtuCallback = std::function<void(uint16_t mtu)>;
    using DisconnectCallback = std::function<void()>;

    static McpBle& getInstance();

    void setConfig(const BleServerConfig& config);
    const BleServerConfig& getConfig() const;

    void init(const std::string& deviceName = "");

    // Stop advertising and drop the active connection (if any). Advertising is
    // NOT restarted by the resulting disconnect event; call init() to resume.
    void stop();

    // Callback setters synchronize with in-flight callback invocations: on
    // return, the previous callback is guaranteed not to be executing on the
    // NimBLE host task and will never run again.
    void setRxCallback(RxCallback cb);
    void setMtuCallback(MtuCallback cb);
    void setDisconnectCallback(DisconnectCallback cb);
    bool sendNotification(const uint8_t* data, size_t len);
    uint16_t getMtu() const;
    bool isConnected() const;
    uint16_t activeConnHandle() const;

    // Internal usage
    bool _onConnect(NimBLEServer* pServer);
    bool _onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo);
    void _onDisconnect(NimBLEServer* pServer);
    void _onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason);
    void _onMtuChange(uint16_t mtu);
    void _onMtuChange(uint16_t mtu, NimBLEConnInfo& connInfo);
    void _onWrite(NimBLECharacteristic* pCharacteristic);
    void _onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo);

private:
    McpBle();
    ~McpBle() = default;
    McpBle(const McpBle&) = delete;
    McpBle& operator=(const McpBle&) = delete;

    BleServerConfig _config;
    RxCallback _rxCallback;
    MtuCallback _mtuCallback;
    DisconnectCallback _disconnectCallback;
    std::atomic<uint16_t> _mtu{23};
    std::atomic<uint16_t> _activeConnHandle{BLE_HS_CONN_HANDLE_NONE};
    bool _initialized = false;
    std::atomic<bool> _stopped{false};

    // Serializes callback invocation (NimBLE host task) against callback
    // reassignment (user task), so teardown can quiesce the host task before
    // freeing the resources those callbacks touch.
    SemaphoreHandle_t _cbMutex = nullptr;
    NimBLEServer* _pServer = nullptr;
    NimBLECharacteristic* _pTxCharacteristic = nullptr;

    const char* SERVICE_UUID = "00001999-0000-1000-8000-00805F9B34FB";
    const char* RX_UUID = "4963505F-5258-4000-8000-00805F9B34FB";
    const char* TX_UUID = "4963505F-5458-4000-8000-00805F9B34FB";
};

#endif  // __has_include(<NimBLEDevice.h>)
