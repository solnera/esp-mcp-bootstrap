#if __has_include(<NimBLEDevice.h>)

#include "McpBle.h"

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        McpBle::getInstance()._onConnect(pServer);
        const auto& cfg = McpBle::getInstance().getConfig();
        pServer->updateConnParams(connInfo.getConnHandle(),
                                  cfg.connMinInterval,
                                  cfg.connMaxInterval,
                                  cfg.connSlaveLatency,
                                  cfg.connSupervisionTimeout);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        (void)connInfo;
        (void)reason;
        McpBle::getInstance()._onDisconnect(pServer);
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        McpBle::getInstance()._onMtuChange(MTU);
    }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
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
    NimBLEDevice::setPowerLevel(_config.txPower, ESP_BLE_PWR_TYPE_DEFAULT);
    NimBLEDevice::setPowerLevel(_config.advTxPower, ESP_BLE_PWR_TYPE_ADV);

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
    // NimBLE 2.x no longer advertises the name from NimBLEDevice::init(), and
    // NimBLEAdvertising::setName() only routes the name into the scan-response
    // packet when scan response is already enabled (m_scanResp). The 128-bit
    // service UUID fills the 31-byte main packet, so the name has to live in the
    // scan response — enable it BEFORE setName(), or setName() fails and the
    // device advertises unnamed.
    pAdvertising->enableScanResponse(_config.scanResponse);
    if (!pAdvertising->setName(name)) {
        Serial.println("[McpBle] Advertised name did not fit; device will be unnamed");
    }
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

void McpBle::setDisconnectCallback(DisconnectCallback cb) {
    _disconnectCallback = cb;
}

bool McpBle::sendNotification(const uint8_t* data, size_t len) {
    if (!_connected || !_pTxCharacteristic) return false;
    return _pTxCharacteristic->notify(data, len);
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
    if (_disconnectCallback) {
        _disconnectCallback();
    }
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
