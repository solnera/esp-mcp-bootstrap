#pragma once

#if __has_include(<NimBLEDevice.h>)

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <functional>

class McpBle {
public:
    using RxCallback = std::function<void(const uint8_t* data, size_t len)>;
    using MtuCallback = std::function<void(uint16_t mtu)>;

    static McpBle& getInstance();

    void init(const std::string& deviceName = "MCP_Server_BLE");
    void setRxCallback(RxCallback cb);
    void setMtuCallback(MtuCallback cb);
    bool sendNotification(const uint8_t* data, size_t len);
    uint16_t getMtu() const;
    bool isConnected() const;

    // Internal usage
    void _onConnect(NimBLEServer* pServer);
    void _onDisconnect(NimBLEServer* pServer);
    void _onMtuChange(uint16_t mtu);
    void _onWrite(NimBLECharacteristic* pCharacteristic);

private:
    McpBle();
    ~McpBle() = default;
    McpBle(const McpBle&) = delete;
    McpBle& operator=(const McpBle&) = delete;

    RxCallback _rxCallback;
    MtuCallback _mtuCallback;
    uint16_t _mtu = 23;
    bool _connected = false;
    bool _initialized = false;
    NimBLEServer* _pServer = nullptr;
    NimBLECharacteristic* _pTxCharacteristic = nullptr;

    const char* SERVICE_UUID = "00001999-0000-1000-8000-00805F9B34FB";
    const char* RX_UUID = "4963505F-5258-4000-8000-00805F9B34FB";
    const char* TX_UUID = "4963505F-5458-4000-8000-00805F9B34FB";
};

#endif  // __has_include(<NimBLEDevice.h>)
