#include "BLECentralTransport.hpp"

// Define static constexpr members for pre-C++17 ODR compliance
constexpr const char* BLECentralTransport::NUS_SERVICE_UUID;
constexpr const char* BLECentralTransport::NUS_TX_CHAR_UUID;
constexpr const char* BLECentralTransport::NUS_RX_CHAR_UUID;

// Static instance pointer for callbacks
BLECentralTransport* BLECentralTransport::s_instance = nullptr;

// Client callbacks class for BLE connection events
class BLECentralTransport::ClientCallbacks : public BLEClientCallbacks {
public:
    explicit ClientCallbacks(BLECentralTransport* transport) : m_transport(transport) {}

    void onConnect(BLEClient* client) override {
        Serial.println("BLE ClientCallbacks: onConnect triggered");
        m_transport->m_connected = true;
        m_transport->m_lastError = "";
        if (m_transport->m_stateCallback) {
            m_transport->m_stateCallback(true);
        }
    }

    void onDisconnect(BLEClient* client) override {
        Serial.println("BLE ClientCallbacks: onDisconnect triggered");
        m_transport->m_connected = false;
        m_transport->m_txCharacteristic = nullptr;
        m_transport->m_rxCharacteristic = nullptr;
        if (m_transport->m_stateCallback) {
            m_transport->m_stateCallback(false);
        }
    }

private:
    BLECentralTransport* m_transport;
};

// Security callbacks class for BLE pairing/bonding
class BLECentralTransport::SecurityCallbacks : public BLESecurityCallbacks {
public:
    explicit SecurityCallbacks(uint32_t pin) : m_pin(pin) {}

    // Called when the peripheral requests a passkey from us (Central with keyboard)
    uint32_t onPassKeyRequest() override {
        Serial.printf("BLE Security: Passkey requested, providing PIN: %lu\n", m_pin);
        return m_pin;
    }

    // Called when we should display a passkey (Central with display)
    void onPassKeyNotify(uint32_t pass_key) override {
        Serial.printf("BLE Security: Passkey to display: %lu\n", pass_key);
    }

    // Called to ask if we should accept security request from peripheral
    bool onSecurityRequest() override {
        Serial.println("BLE Security: Security request received, accepting");
        return true;
    }

    // Called when authentication is complete
    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
        if (auth_cmpl.success) {
            Serial.println("BLE Security: Authentication complete - SUCCESS");
            Serial.printf("  Auth mode: %d, Key type: %d\n", 
                          auth_cmpl.auth_mode, auth_cmpl.key_type);
        } else {
            Serial.printf("BLE Security: Authentication FAILED - reason: %d\n", 
                          auth_cmpl.fail_reason);
        }
    }

    // Called to confirm if displayed passkey matches
    bool onConfirmPIN(uint32_t pin) override {
        Serial.printf("BLE Security: Confirm PIN %lu? Accepting.\n", pin);
        return true;
    }

private:
    uint32_t m_pin;
};

BLECentralTransport::BLECentralTransport(const Config& config)
    : m_config(config)
{
    s_instance = this;
    m_clientCallbacks = new ClientCallbacks(this);
    m_securityCallbacks = new SecurityCallbacks(config.pairingPin);
}

BLECentralTransport::~BLECentralTransport() {
    deinit();
    delete m_clientCallbacks;
    delete m_securityCallbacks;
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

bool BLECentralTransport::init() {
    Serial.println("BLECentralTransport: Initializing BLE...");
    Serial.printf("Free heap before BLE init: %d bytes\n", ESP.getFreeHeap());

    BLEDevice::init(m_config.deviceName);

    if (!BLEDevice::getInitialized()) {
        Serial.println("ERROR: BLE initialization failed - check if Bluetooth is enabled in sdkconfig");
        Serial.printf("Free heap after failed init: %d bytes\n", ESP.getFreeHeap());
        return false;
    }

    m_bleInitialized = true;
    Serial.printf("BLE initialized successfully as '%s'\n", m_config.deviceName);
    Serial.printf("Free heap after BLE init: %d bytes\n", ESP.getFreeHeap());

    // Set up security callbacks to handle pairing with MeshCore device
    // This is essential for the peripheral to recognize us as properly connected
    BLEDevice::setSecurityCallbacks(m_securityCallbacks);
    
    // Configure BLE security for Central role connecting to a secured peripheral
    // ESP_IO_CAP_KBDISP = Keyboard + Display (we can input and display PINs)
    // This allows us to respond to passkey requests from the MeshCore device
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    
    BLESecurity* security = new BLESecurity();
    security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    security->setCapability(ESP_IO_CAP_KBDISP);
    security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    
    Serial.printf("BLE Security configured with PIN: %lu\n", m_config.pairingPin);

    return true;
}

void BLECentralTransport::deinit() {
    disconnect();
    
    if (m_bleInitialized) {
        delay(m_config.deinitCleanupDelayMs);
        BLEDevice::deinit();
        m_bleInitialized = false;
        Serial.println("BLE deinitialized");
    }
}

bool BLECentralTransport::send(const uint8_t* data, size_t len) {
    if (!m_connected || m_txCharacteristic == nullptr) {
        return false;
    }

    try {
        // Copy data to non-const buffer as BLE library expects mutable data
        // This ensures we don't violate the const contract of the interface
        std::vector<uint8_t> buffer(data, data + len);
        // Use Write With Response for reliable protocol commands
        m_txCharacteristic->writeValue(buffer.data(), buffer.size(), true);
        return true;
    } catch (...) {
        Serial.println("BLECentralTransport: write error");
        return false;
    }
}

bool BLECentralTransport::isConnected() const {
    return m_connected;
}

void BLECentralTransport::setRxCallback(RxCallback callback) {
    m_rxCallback = std::move(callback);
}

void BLECentralTransport::setStateCallback(StateCallback callback) {
    m_stateCallback = std::move(callback);
}

void BLECentralTransport::notifyCallback(BLERemoteCharacteristic* pCharacteristic,
                                          uint8_t* pData, size_t length, bool isNotify) {
    if (s_instance && s_instance->m_rxCallback && length > 0) {
        s_instance->m_rxCallback(pData, length);
    }
}

void BLECentralTransport::scanDevices() {
    Serial.println("========================================");
    Serial.println("Starting BLE device scan...");
    Serial.println("========================================");

    if (!m_bleInitialized) {
        if (!init()) {
            Serial.println("ERROR: Cannot scan - BLE initialization failed");
            return;
        }
    }

    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);

    const int scanSeconds = 5;
    Serial.printf("Scanning for BLE devices for %d seconds...\n", scanSeconds);

    BLEScanResults results = scan->start(scanSeconds, false);
    int deviceCount = results.getCount();

    if (deviceCount < 0) {
        Serial.println("BLE scan failed");
        scan->clearResults();
        return;
    }

    Serial.println("========================================");
    Serial.printf("BLE Scan Complete: %d device(s) found\n", deviceCount);
    Serial.println("========================================");

    for (int i = 0; i < deviceCount; i++) {
        BLEAdvertisedDevice device = results.getDevice(i);

        std::string rawName = device.getName();
        String devName = rawName.length() ? String(rawName.c_str()) : String("(unnamed)");
        String devAddr = String(device.getAddress().toString().c_str());
        int rssi = device.getRSSI();

        Serial.printf("[%d] Name: %s\n", i + 1, devName.c_str());
        Serial.printf("    Address: %s\n", devAddr.c_str());
        Serial.printf("    RSSI: %d dBm\n", rssi);

        if (device.haveServiceUUID()) {
            BLEUUID serviceUUID = device.getServiceUUID();
            Serial.printf("    Service UUID: %s\n", serviceUUID.toString().c_str());
        }

        Serial.println("----------------------------------------");
    }

    scan->clearResults();
    Serial.println("BLE scan complete");
}

bool BLECentralTransport::connect() {
    m_connected = false;
    m_txCharacteristic = nullptr;
    m_rxCharacteristic = nullptr;

    if (!m_bleInitialized) {
        if (!init()) {
            m_lastError = "BLE initialization failed";
            return false;
        }
    }

    Serial.printf("Scanning for MeshCore peer named '%s'...\n", m_config.peerName);

    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(200);
    scan->setWindow(160);

    BLEScanResults results = scan->start(m_config.scanSeconds, false);
    Serial.printf("Scan complete: %d devices found\n", results.getCount());

    for (int i = 0; i < results.getCount(); i++) {
        BLEAdvertisedDevice device = results.getDevice(i);

        std::string rawName = device.getName();
        String devName = rawName.length() ? String(rawName.c_str()) : String("");
        String devAddr = String(device.getAddress().toString().c_str());
        int rssi = device.getRSSI();
        Serial.printf("  [%d] Name='%s' Addr=%s RSSI=%d\n", i, devName.c_str(), devAddr.c_str(), rssi);

        if (device.haveServiceUUID()) {
            BLEUUID adv = device.getServiceUUID();
            Serial.printf("       Advertised service UUID: %s\n", adv.toString().c_str());
        }

        // Matching logic
        bool nameMatches = (devName.length() && devName == String(m_config.peerName));
        bool nameContains = (devName.length() && devName.indexOf(String(m_config.peerName)) >= 0);
        bool serviceMatches = false;

        if (device.haveServiceUUID()) {
            BLEUUID adv = device.getServiceUUID();
            serviceMatches = adv.equals(BLEUUID(NUS_SERVICE_UUID));
        }

        if (!(nameMatches || nameContains || serviceMatches)) {
            continue;
        }

        Serial.println("MeshCore peer candidate found, attempting connection...");

        // Clean up any existing client
        if (m_client != nullptr) {
            if (m_client->isConnected()) {
                m_client->disconnect();
            }
            delete m_client;
            m_client = nullptr;
            delay(m_config.clientCleanupDelayMs);
        }

        m_client = BLEDevice::createClient();
        m_client->setClientCallbacks(m_clientCallbacks);

        BLEAddress peerAddress = device.getAddress();
        esp_ble_addr_type_t addrType = device.getAddressType();

        if (!m_client->connect(peerAddress, addrType)) {
            m_lastError = "Connection attempt failed";
            Serial.println("Connection attempt failed");
            continue;
        }

        delay(m_config.mtuNegotiationDelayMs);

        // Service discovery with retries
        BLERemoteService* service = nullptr;
        for (int retry = 0; retry < m_config.serviceDiscoveryRetries; retry++) {
            service = m_client->getService(NUS_SERVICE_UUID);
            if (service != nullptr) {
                Serial.printf("Found MeshCore service on attempt %d\n", retry + 1);
                break;
            }

            auto* discoveredServices = m_client->getServices();
            if (discoveredServices != nullptr && !discoveredServices->empty()) {
                Serial.printf("Service discovery attempt %d: found %d service(s)\n", 
                              retry + 1, discoveredServices->size());
                for (auto& svc : *discoveredServices) {
                    if (BLEUUID(svc.first).equals(BLEUUID(NUS_SERVICE_UUID))) {
                        service = svc.second;
                        break;
                    }
                }
                if (service != nullptr) break;
            }

            if (retry < m_config.serviceDiscoveryRetries - 1) {
                delay(m_config.serviceDiscoveryRetryDelayMs);
            }
        }

        if (service == nullptr) {
            m_lastError = "Nordic UART Service not found on peer";
            Serial.println("Nordic UART Service not found on peer, disconnecting...");
            m_client->disconnect();
            continue;
        }

        // Get TX characteristic
        BLERemoteCharacteristic* txChar = service->getCharacteristic(NUS_TX_CHAR_UUID);
        if (txChar == nullptr || !txChar->canWrite()) {
            m_lastError = "NUS TX characteristic missing or not writable";
            Serial.println(m_lastError.c_str());
            m_client->disconnect();
            continue;
        }

        // Get RX characteristic
        BLERemoteCharacteristic* rxChar = service->getCharacteristic(NUS_RX_CHAR_UUID);
        if (rxChar == nullptr || !rxChar->canNotify()) {
            m_lastError = "NUS RX characteristic missing or cannot notify";
            Serial.println(m_lastError.c_str());
            m_client->disconnect();
            continue;
        }

        // Register for notifications with explicit parameters
        // Parameters: callback, notifications=true, descriptorRequiresRegistration=true
        // The ESP-IDF library requires calling esp_ble_gattc_register_for_notify before
        // notifications will be received, and also writing to the CCCD descriptor.
        rxChar->registerForNotify(notifyCallback, true, true);
        
        // Some MeshCore devices require an additional CCCD write after registration
        // to properly enable notifications. This redundant write ensures compatibility.
        BLERemoteDescriptor* cccd = rxChar->getDescriptor(BLEUUID((uint16_t)0x2902));
        if (cccd != nullptr) {
            uint8_t notifyOn[] = {0x01, 0x00};
            cccd->writeValue(notifyOn, sizeof(notifyOn), true);
            Serial.println("CCCD notification bit enabled");
        } else {
            Serial.println("Warning: CCCD descriptor not found");
        }
        
        // Wait for notification registration to complete
        delay(m_config.notifyRegistrationDelayMs);

        m_txCharacteristic = txChar;
        m_rxCharacteristic = rxChar;
        m_connected = true;
        m_lastError = "";
        
        Serial.println("Connected to MeshCore peer via Nordic UART Service");
        scan->clearResults();
        return true;
    }

    scan->clearResults();
    m_lastError = "MeshCore peer not found during scan";
    Serial.println(m_lastError.c_str());
    return false;
}

void BLECentralTransport::disconnect() {
    if (m_client != nullptr) {
        if (m_client->isConnected()) {
            Serial.println("Disconnecting from MeshCore...");
            m_client->disconnect();
        }
        delete m_client;
        m_client = nullptr;
    }

    m_connected = false;
    m_txCharacteristic = nullptr;
    m_rxCharacteristic = nullptr;
}
