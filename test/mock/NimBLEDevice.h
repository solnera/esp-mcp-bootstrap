#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

/* Test instrumentation for the notify() path: tracks how many notifications run
 * concurrently (to prove sends are serialized) and a total call count. */
namespace mock_ble {
inline std::atomic<int>& notifyActive() { static std::atomic<int> a{0}; return a; }
inline std::atomic<int>& notifyMaxConcurrent() { static std::atomic<int> m{0}; return m; }
inline std::atomic<int>& notifyCount() { static std::atomic<int> c{0}; return c; }
inline std::atomic<int>& notifyFailCountdown() { static std::atomic<int> f{0}; return f; }
inline std::atomic<int>& lastNotifyConnHandle() { static std::atomic<int> h{-1}; return h; }
inline std::atomic<int>& disconnectCount() { static std::atomic<int> c{0}; return c; }
inline std::atomic<int>& lastDisconnectConnHandle() { static std::atomic<int> h{-1}; return h; }
inline std::atomic<int>& stopAdvertisingCount() { static std::atomic<int> c{0}; return c; }
inline std::atomic<int>& startAdvertisingCount() { static std::atomic<int> c{0}; return c; }
// Last preferred MTU offered via NimBLEDevice::setMTU(); -1 if never called.
inline std::atomic<int>& lastPreferredMtu() { static std::atomic<int> m{-1}; return m; }
inline std::vector<uint8_t>& lastNotifyPayload() { static std::vector<uint8_t> p; return p; }
inline void failNextNotify(int n) { notifyFailCountdown().store(n); }
inline void resetNotifyTracking() {
    notifyActive().store(0);
    notifyMaxConcurrent().store(0);
    notifyCount().store(0);
    notifyFailCountdown().store(0);
    lastNotifyConnHandle().store(-1);
    lastNotifyPayload().clear();
}
inline void resetConnectionTracking() {
    disconnectCount().store(0);
    lastDisconnectConnHandle().store(-1);
    stopAdvertisingCount().store(0);
    startAdvertisingCount().store(0);
}
// Last name pushed onto the advertisement via NimBLEAdvertising::setName().
inline std::string& advertisedName() { static std::string s; return s; }
}  // namespace mock_ble

typedef int esp_power_level_t;
typedef int esp_ble_power_type_t;
constexpr esp_power_level_t ESP_PWR_LVL_P3 = 3;
constexpr esp_ble_power_type_t ESP_BLE_PWR_TYPE_DEFAULT = 0;
constexpr esp_ble_power_type_t ESP_BLE_PWR_TYPE_ADV = 1;
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xFFFF;
constexpr uint8_t BLE_ERR_REM_USER_CONN_TERM = 0x13;

class NimBLEServer;
class NimBLECharacteristic;

// Mirrors NimBLE 2.x: connection info passed to server/characteristic callbacks.
class NimBLEConnInfo {
public:
    NimBLEConnInfo() = default;
    explicit NimBLEConnInfo(uint16_t handle) : handle_(handle) {}
    uint16_t getConnHandle() const { return handle_; }

private:
    uint16_t handle_ = 0;
};

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
    virtual void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) {
        (void)server;
        (void)connInfo;
    }
    virtual void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) {
        (void)server;
        (void)connInfo;
        (void)reason;
    }
    virtual void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) {
        (void)mtu;
        (void)connInfo;
    }
};

class NimBLECharacteristicCallbacks {
public:
    virtual ~NimBLECharacteristicCallbacks() = default;
    virtual void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) {
        (void)characteristic;
        (void)connInfo;
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
    bool notify(const uint8_t* data, size_t len, uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE) {
        mock_ble::lastNotifyPayload().assign(data, data + len);
        mock_ble::lastNotifyConnHandle().store(static_cast<int>(connHandle));
        mock_ble::notifyCount().fetch_add(1);
        int active = mock_ble::notifyActive().fetch_add(1) + 1;
        int prev = mock_ble::notifyMaxConcurrent().load();
        while (active > prev &&
               !mock_ble::notifyMaxConcurrent().compare_exchange_weak(prev, active)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        mock_ble::notifyActive().fetch_sub(1);
        if (mock_ble::notifyFailCountdown().load() > 0) {
            mock_ble::notifyFailCountdown().fetch_sub(1);
            return false;
        }
        return true;
    }
    NimBLEAttValue getValue() const {
        return NimBLEAttValue(value_);
    }
    void setValue(const std::vector<uint8_t>& value) {
        value_ = value;
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
    bool disconnect(uint16_t connHandle, uint8_t reason = BLE_ERR_REM_USER_CONN_TERM) {
        (void)reason;
        mock_ble::disconnectCount().fetch_add(1);
        mock_ble::lastDisconnectConnHandle().store(static_cast<int>(connHandle));
        return true;
    }
    bool disconnect(const NimBLEConnInfo& connInfo, uint8_t reason = BLE_ERR_REM_USER_CONN_TERM) {
        return disconnect(connInfo.getConnHandle(), reason);
    }
    bool stopAdvertising() {
        mock_ble::stopAdvertisingCount().fetch_add(1);
        return true;
    }
    bool startAdvertising() {
        mock_ble::startAdvertisingCount().fetch_add(1);
        return true;
    }

private:
    NimBLEServerCallbacks* callbacks_ = nullptr;
};

class NimBLEAdvertising {
public:
    void addServiceUUID(const char* uuid) {
        (void)uuid;
        hasServiceUUID_ = true;
    }
    void enableScanResponse(bool enabled) { scanResp_ = enabled; }
    // Mirrors NimBLE 2.x NimBLEAdvertising::setName: the name is placed into the
    // scan-response packet when scan response is enabled; otherwise into the main
    // advertising packet, which cannot also hold a 128-bit service UUID (18B UUID
    // + name overflows the 31B packet). In that case setName fails and the name
    // is left unset — so enableScanResponse() must be called BEFORE setName().
    bool setName(const std::string& name) {
        if (scanResp_) {
            mock_ble::advertisedName() = name;
            return true;
        }
        if (hasServiceUUID_) {
            return false;  // would overflow the 31-byte main packet
        }
        mock_ble::advertisedName() = name;
        return true;
    }
    void setMinInterval(uint16_t interval) { (void)interval; }
    void setMaxInterval(uint16_t interval) { (void)interval; }
    void start() {}

private:
    bool scanResp_ = false;
    bool hasServiceUUID_ = false;
};

class NimBLEDevice {
public:
    static void init(const std::string& deviceName) { (void)deviceName; }
    static bool setPowerLevel(esp_power_level_t power,
                              esp_ble_power_type_t type = ESP_BLE_PWR_TYPE_DEFAULT) {
        (void)power;
        (void)type;
        return true;
    }
    static NimBLEServer* createServer() {
        static NimBLEServer server;
        return &server;
    }
    static NimBLEAdvertising* getAdvertising() {
        static NimBLEAdvertising advertising;
        return &advertising;
    }
    static bool startAdvertising() {
        mock_ble::startAdvertisingCount().fetch_add(1);
        return true;
    }
    static bool stopAdvertising() {
        mock_ble::stopAdvertisingCount().fetch_add(1);
        return true;
    }
    // Matches NimBLE-Arduino 2.x: bool, true on success.
    static bool setMTU(uint16_t mtu) {
        mock_ble::lastPreferredMtu().store(mtu);
        return true;
    }
};
