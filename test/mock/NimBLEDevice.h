#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr int ESP_PWR_LVL_P3 = 3;

struct ble_gap_conn_desc {
    uint16_t conn_handle = 0;
};
class NimBLEServer;
class NimBLECharacteristic;

class NimBLEAttValue {
public:
    NimBLEAttValue() = default;
    explicit NimBLEAttValue(const std::vector<uint8_t>& value) : value_(value) {}

    size_t size() const { return value_.size(); }
    const uint8_t* data() const { return value_.data(); }

private:
    std::vector<uint8_t> value_;
};

class NimBLEServerCallbacks {
public:
    virtual ~NimBLEServerCallbacks() = default;
    virtual void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) {
        (void)server;
        (void)desc;
    }
    virtual void onDisconnect(NimBLEServer* server) {
        (void)server;
    }
    virtual void onMTUChange(uint16_t mtu, ble_gap_conn_desc* desc) {
        (void)mtu;
        (void)desc;
    }
};

class NimBLECharacteristicCallbacks {
public:
    virtual ~NimBLECharacteristicCallbacks() = default;
    virtual void onWrite(NimBLECharacteristic* characteristic) {
        (void)characteristic;
    }
};

namespace NIMBLE_PROPERTY {
constexpr uint32_t WRITE = 0x01;
constexpr uint32_t WRITE_NR = 0x02;
constexpr uint32_t NOTIFY = 0x04;
}  // namespace NIMBLE_PROPERTY

class NimBLECharacteristic {
public:
    void setCallbacks(NimBLECharacteristicCallbacks* callbacks) {
        callbacks_ = callbacks;
    }
    void notify(const uint8_t* data, size_t len) {
        (void)data;
        (void)len;
    }
    NimBLEAttValue getValue() const {
        return NimBLEAttValue(value_);
    }

private:
    NimBLECharacteristicCallbacks* callbacks_ = nullptr;
    std::vector<uint8_t> value_;
};

class NimBLEService {
public:
    NimBLECharacteristic* createCharacteristic(const char* uuid, uint32_t properties) {
        (void)uuid;
        (void)properties;
        return new NimBLECharacteristic();
    }
    void start() {}
};

class NimBLEServer {
public:
    void setCallbacks(NimBLEServerCallbacks* callbacks) {
        callbacks_ = callbacks;
    }
    NimBLEService* createService(const char* uuid) {
        (void)uuid;
        return new NimBLEService();
    }
    void updateConnParams(uint16_t conn_handle, uint16_t min_interval, uint16_t max_interval,
                          uint16_t latency, uint16_t timeout) {
        (void)conn_handle;
        (void)min_interval;
        (void)max_interval;
        (void)latency;
        (void)timeout;
    }

private:
    NimBLEServerCallbacks* callbacks_ = nullptr;
};

class NimBLEAdvertising {
public:
    void addServiceUUID(const char* uuid) { (void)uuid; }
    void setScanResponse(bool enabled) { (void)enabled; }
    void setMinInterval(uint16_t interval) { (void)interval; }
    void setMaxInterval(uint16_t interval) { (void)interval; }
    void start() {}
};

class NimBLEDevice {
public:
    static void init(const std::string& deviceName) { (void)deviceName; }
    static void setPower(int power) { (void)power; }
    static NimBLEServer* createServer() {
        static NimBLEServer server;
        return &server;
    }
    static NimBLEAdvertising* getAdvertising() {
        static NimBLEAdvertising advertising;
        return &advertising;
    }
    static void startAdvertising() {}
};
