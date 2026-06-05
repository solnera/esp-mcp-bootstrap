#pragma once

#if __has_include(<NimBLEDevice.h>)

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <functional>
#include <string>

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
    uint16_t _mtu = 23;
    bool _connected = false;
    uint16_t _activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
    bool _initialized = false;
    NimBLEServer* _pServer = nullptr;
    NimBLECharacteristic* _pTxCharacteristic = nullptr;

    const char* SERVICE_UUID = "00001999-0000-1000-8000-00805F9B34FB";
    const char* RX_UUID = "4963505F-5258-4000-8000-00805F9B34FB";
    const char* TX_UUID = "4963505F-5458-4000-8000-00805F9B34FB";
};

#endif  // __has_include(<NimBLEDevice.h>)
