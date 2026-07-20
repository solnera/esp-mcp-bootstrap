#if __has_include(<NimBLEDevice.h>)

#include "McpBle.h"

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        if (!McpBle::getInstance()._onConnect(pServer, connInfo)) {
            return;
        }
        const auto& cfg = McpBle::getInstance().getConfig();
        pServer->updateConnParams(connInfo.getConnHandle(),
                                  cfg.connMinInterval,
                                  cfg.connMaxInterval,
                                  cfg.connSlaveLatency,
                                  cfg.connSupervisionTimeout);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        McpBle::getInstance()._onDisconnect(pServer, connInfo, reason);
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
        McpBle::getInstance()._onMtuChange(MTU, connInfo);
    }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        McpBle::getInstance()._onWrite(pCharacteristic, connInfo);
    }
};

McpBle& McpBle::getInstance() {
    static McpBle instance;
    return instance;
}

McpBle::McpBle() {
    _cbMutex = xSemaphoreCreateMutex();
}

namespace {

/* RAII guard for _cbMutex. Tolerates a null mutex (allocation failure) by
 * degrading to the previous unguarded behavior. */
class CbLock {
public:
    explicit CbLock(SemaphoreHandle_t mutex) : _mutex(mutex) {
        if (_mutex) {
            xSemaphoreTake(_mutex, portMAX_DELAY);
        }
    }
    ~CbLock() {
        if (_mutex) {
            xSemaphoreGive(_mutex);
        }
    }
    CbLock(const CbLock&) = delete;
    CbLock& operator=(const CbLock&) = delete;

private:
    SemaphoreHandle_t _mutex;
};

}  // namespace

void McpBle::setConfig(const BleServerConfig& config) {
    _config = config;
}

const BleServerConfig& McpBle::getConfig() const {
    return _config;
}

void McpBle::init(const std::string& deviceName) {
    if (_initialized) {
        _stopped = false;
        NimBLEDevice::startAdvertising();
        return;
    }
    _stopped = false;

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

void McpBle::stop() {
    if (!_initialized) {
        return;
    }
    /* _stopped is set under _cbMutex and _onDisconnect re-checks it under the
     * same mutex before restarting advertising, so a peer-initiated disconnect
     * racing stop() either sees the flag, or its restart is ordered before the
     * stopAdvertising() below — which then wins. The mutex is NOT held across
     * the NimBLE calls (only the host task may take _cbMutex and then NimBLE's
     * internal locks; taking them in the other order here could deadlock). */
    {
        CbLock lock(_cbMutex);
        _stopped = true;
    }
    NimBLEDevice::stopAdvertising();
    if (_connected && _pServer) {
        _pServer->disconnect(_activeConnHandle);
    }
}

void McpBle::setRxCallback(RxCallback cb) {
    CbLock lock(_cbMutex);
    _rxCallback = cb;
}

void McpBle::setMtuCallback(MtuCallback cb) {
    CbLock lock(_cbMutex);
    _mtuCallback = cb;
}

void McpBle::setDisconnectCallback(DisconnectCallback cb) {
    CbLock lock(_cbMutex);
    _disconnectCallback = cb;
}

bool McpBle::sendNotification(const uint8_t* data, size_t len) {
    if (!_connected || !_pTxCharacteristic) return false;
    return _pTxCharacteristic->notify(data, len, _activeConnHandle);
}

uint16_t McpBle::getMtu() const {
    return _mtu;
}

bool McpBle::isConnected() const {
    return _connected;
}

uint16_t McpBle::activeConnHandle() const {
    return _activeConnHandle;
}

bool McpBle::_onConnect(NimBLEServer* pServer) {
    _connected = true;
    _activeConnHandle = 0;
    if (pServer) {
        pServer->stopAdvertising();
    } else {
        NimBLEDevice::stopAdvertising();
    }
    return true;
}

void McpBle::_onDisconnect(NimBLEServer* pServer) {
    (void)pServer;
    _connected = false;
    _activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
    _mtu = 23;  // Reset MTU
    {
        CbLock lock(_cbMutex);
        /* Propagate the ATT-default MTU to listeners. Without this the
         * transport keeps the previous connection's negotiated MTU and frames
         * the next connection's pre-negotiation traffic too large to notify. */
        if (_mtuCallback) {
            _mtuCallback(_mtu);
        }
        if (_disconnectCallback) {
            _disconnectCallback();
        }
        /* Checked under _cbMutex so it cannot race stop() setting the flag —
         * otherwise a peer disconnect landing during end() could restart
         * advertising after teardown completed. */
        if (!_stopped) {
            NimBLEDevice::startAdvertising();
        }
    }
}

bool McpBle::_onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    const uint16_t connHandle = connInfo.getConnHandle();
    {
        /* A connection the controller accepted just before stop() took effect
         * can be delivered after teardown; admitting it would strand the
         * central on a server whose callbacks are gone. Drop it. */
        CbLock lock(_cbMutex);
        if (_stopped) {
            if (pServer) {
                pServer->disconnect(connHandle);
            }
            return false;
        }
    }
    if (_connected && _activeConnHandle != connHandle) {
        if (pServer) {
            pServer->disconnect(connHandle);
        }
        return false;
    }

    _connected = true;
    _activeConnHandle = connHandle;
    if (pServer) {
        pServer->stopAdvertising();
    } else {
        NimBLEDevice::stopAdvertising();
    }
    return true;
}

void McpBle::_onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    (void)reason;
    if (!_connected || connInfo.getConnHandle() != _activeConnHandle) {
        return;
    }
    _onDisconnect(pServer);
}

void McpBle::_onMtuChange(uint16_t mtu) {
    _mtu = mtu;
    CbLock lock(_cbMutex);
    if (_mtuCallback) {
        _mtuCallback(_mtu);
    }
}

void McpBle::_onMtuChange(uint16_t mtu, NimBLEConnInfo& connInfo) {
    if (!_connected || connInfo.getConnHandle() != _activeConnHandle) {
        return;
    }
    _onMtuChange(mtu);
}

void McpBle::_onWrite(NimBLECharacteristic* pCharacteristic) {
    CbLock lock(_cbMutex);
    if (_rxCallback) {
        NimBLEAttValue value = pCharacteristic->getValue();
        if (value.size() > 0) {
            _rxCallback(value.data(), value.size());
        }
    }
}

void McpBle::_onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    if (!_connected || connInfo.getConnHandle() != _activeConnHandle) {
        return;
    }
    _onWrite(pCharacteristic);
}

#endif  // __has_include(<NimBLEDevice.h>)
