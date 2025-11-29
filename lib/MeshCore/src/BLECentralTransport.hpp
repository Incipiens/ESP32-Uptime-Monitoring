#pragma once

#include "IByteTransport.hpp"
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <Arduino.h>

/**
 * BLECentralTransport - Low-level BLE transport layer
 * 
 * This class handles:
 * - ESP-IDF BLE initialization (NVS, controller, Bluedroid)
 * - Scanning for the companion device by name and/or service UUID
 * - Connecting as a BLE Central
 * - Discovering the SerialBLEInterface GATT service and its TX/RX characteristics
 * - Enabling notifications on RX
 * 
 * Implements IByteTransport interface for use by higher layers.
 */
class BLECentralTransport : public IByteTransport {
public:
    // Nordic UART Service UUIDs (used by MeshCore BLE companion firmware)
    static constexpr const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    static constexpr const char* NUS_TX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // Write to this
    static constexpr const char* NUS_RX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // Notifications from this
    
    // Preferred MTU size for BLE connection.
    // MeshCore channel info responses are ~50 bytes (cmd + index + 32-byte name + 16-byte secret).
    // Request larger MTU to receive complete responses in single BLE notifications instead of
    // having them fragmented. With 3-byte ATT header overhead, we need MTU > 53 for channel info.
    // 185 bytes provides margin for larger messages and matches typical BLE 4.2+ capabilities.
    static constexpr uint16_t PREFERRED_MTU_SIZE = 185;

    // Configuration for BLE timing
    struct Config {
        const char* deviceName = "ESP32-Uptime";
        const char* peerName = "";
        uint32_t pairingPin = 123456;
        int mtuNegotiationDelayMs = 2000;
        int serviceDiscoveryRetries = 5;
        int serviceDiscoveryRetryDelayMs = 1000;
        int deinitCleanupDelayMs = 100;
        int clientCleanupDelayMs = 100;
        int notifyRegistrationDelayMs = 500;
        int scanSeconds = 10;
    };

    explicit BLECentralTransport(const Config& config);
    ~BLECentralTransport() override;

    // IByteTransport interface
    bool send(const uint8_t* data, size_t len) override;
    bool isConnected() const override;
    void setRxCallback(RxCallback callback) override;
    void setStateCallback(StateCallback callback) override;

    // BLE-specific methods
    
    /**
     * Initialize BLE stack
     * @return true if initialization succeeded
     */
    bool init();

    /**
     * Deinitialize BLE stack and free resources
     */
    void deinit();

    /**
     * Check if BLE is initialized
     * @return true if initialized
     */
    bool isInitialized() const { return m_bleInitialized; }

    /**
     * Connect to the MeshCore peer device
     * @return true if connection succeeded
     */
    bool connect();

    /**
     * Disconnect from peer device
     */
    void disconnect();

    /**
     * Scan for BLE devices and log results (diagnostic)
     */
    void scanDevices();

    /**
     * Get last error message
     * @return Last error message string
     */
    const String& getLastError() const { return m_lastError; }

private:
    class ClientCallbacks;
    class SecurityCallbacks;
    
    // Static notification callback for BLE
    static void notifyCallback(BLERemoteCharacteristic* pCharacteristic, 
                               uint8_t* pData, size_t length, bool isNotify);

    // Static instance pointer for callbacks (since BLE library requires static callbacks)
    // NOTE: This design limits to one active BLECentralTransport instance at a time.
    // For multi-device scenarios, consider using a callback registry with instance lookup.
    // For this single-device application, the singleton pattern is acceptable.
    static BLECentralTransport* s_instance;

    Config m_config;
    bool m_bleInitialized = false;
    bool m_connected = false;
    
    BLEClient* m_client = nullptr;
    BLERemoteCharacteristic* m_txCharacteristic = nullptr;
    BLERemoteCharacteristic* m_rxCharacteristic = nullptr;
    
    RxCallback m_rxCallback;
    StateCallback m_stateCallback;
    
    String m_lastError;
    
    // Client callbacks instance
    ClientCallbacks* m_clientCallbacks = nullptr;
    
    // Security callbacks instance  
    SecurityCallbacks* m_securityCallbacks = nullptr;
};
