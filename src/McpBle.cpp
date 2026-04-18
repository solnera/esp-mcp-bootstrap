#if __has_include(<NimBLEDevice.h>)

#include "McpBle.h"

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        McpBle::getInstance()._onConnect(pServer);
        const auto& cfg = McpBle::getInstance().getConfig();
        pServer->updateConnParams(desc->conn_handle,
                                  cfg.connMinInterval,
                                  cfg.connMaxInterval,
                                  cfg.connSlaveLatency,
                                  cfg.connSupervisionTimeout);
    }

    void onDisconnect(NimBLEServer* pServer) override {
        McpBle::getInstance()._onDisconnect(pServer);
    }

    void onMTUChange(uint16_t MTU, ble_gap_conn_desc* desc) override {
        McpBle::getInstance()._onMtuChange(MTU);
    }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override {
        McpBle::getInstance()._onWrite(pCharacteristic);
    }
};

McpBle& McpBle::getInstance() {
    static McpBle instance;
    return instance;
}

McpBle::McpBle() {}

void McpBle::setConfig(const BleServerConfig& config) {
    _config = config;
}

const BleServerConfig& McpBle::getConfig() const {
    return _config;
}

void McpBle::init(const std::string& deviceName) {
    if (_initialized) {
        NimBLEDevice::startAdvertising();
        return;
    }

    const std::string& name = deviceName.empty() ? _config.deviceName : deviceName;
    NimBLEDevice::init(name);
    NimBLEDevice::setPower(_config.txPower, ESP_BLE_PWR_TYPE_DEFAULT);
    NimBLEDevice::setPower(_config.advTxPower, ESP_BLE_PWR_TYPE_ADV);

    _pServer = NimBLEDevice::createServer();
    _pServer->setCallbacks(new ServerCallbacks());

    NimBLEService* pService = _pServer->createService(SERVICE_UUID);

    // RX Characteristic (Write)
    NimBLECharacteristic* pRxChar = pService->createCharacteristic(
        RX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxChar->setCallbacks(new CharCallbacks());

    // TX Characteristic (Notify)
    _pTxCharacteristic = pService->createCharacteristic(
        TX_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    pService->start();

    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(_config.scanResponse);
    pAdvertising->setMinInterval(_config.advMinInterval);
    pAdvertising->setMaxInterval(_config.advMaxInterval);
    pAdvertising->start();
    _initialized = true;
}

void McpBle::setRxCallback(RxCallback cb) {
    _rxCallback = cb;
}

void McpBle::setMtuCallback(MtuCallback cb) {
    _mtuCallback = cb;
}

bool McpBle::sendNotification(const uint8_t* data, size_t len) {
    if (!_connected || !_pTxCharacteristic) return false;
    _pTxCharacteristic->notify(data, len);
    return true;
}

uint16_t McpBle::getMtu() const {
    return _mtu;
}

bool McpBle::isConnected() const {
    return _connected;
}

void McpBle::_onConnect(NimBLEServer* pServer) {
    _connected = true;
}

void McpBle::_onDisconnect(NimBLEServer* pServer) {
    _connected = false;
    _mtu = 23;  // Reset MTU
    NimBLEDevice::startAdvertising();
}

void McpBle::_onMtuChange(uint16_t mtu) {
    _mtu = mtu;
    if (_mtuCallback) {
        _mtuCallback(_mtu);
    }
}

void McpBle::_onWrite(NimBLECharacteristic* pCharacteristic) {
    if (_rxCallback) {
        NimBLEAttValue value = pCharacteristic->getValue();
        if (value.size() > 0) {
            _rxCallback(value.data(), value.size());
        }
    }
}

#endif  // __has_include(<NimBLEDevice.h>)
