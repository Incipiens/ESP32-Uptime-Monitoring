#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <ESP32Ping.h>
#include <mbedtls/base64.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEUtils.h>

#include "config.hpp"

bool isNtfyConfigured() {
  return strlen(NTFY_TOPIC) > 0;
}

bool isDiscordConfigured() {
  return strlen(DISCORD_WEBHOOK_URL) > 0;
}

bool isSmtpConfigured() {
  return strlen(SMTP_SERVER) > 0 && strlen(SMTP_FROM_ADDRESS) > 0 &&
         strlen(SMTP_TO_ADDRESS) > 0;
}

bool isMeshCoreConfigured() {
  return strlen(BLE_PEER_NAME) > 0;
}

void sendNtfyNotification(const String& title, const String& message, const String& tags = "warning,monitor");
void sendDiscordNotification(const String& title, const String& message);
void sendSmtpNotification(const String& title, const String& message);
void sendMeshCoreNotification(const String& title, const String& message);
void scanBLEDevices();
void initBLE();
bool connectToMeshCore();
void disconnectFromMeshCore();
void disconnectWiFi();
void reconnectWiFi();
bool ensureMeshChannel();
bool provisionMeshChannel();

AsyncWebServer server(80);

// BLE / MeshCore
BLEClient* meshClient = nullptr;
BLERemoteCharacteristic* meshTxCharacteristic = nullptr;  // ESP32 writes to this (TX to device)
BLERemoteCharacteristic* meshRxCharacteristic = nullptr;  // ESP32 reads from this (RX from device)
bool meshDeviceConnected = false;
unsigned long lastMeshConnectAttempt = 0;
String lastMeshError = "";
bool meshChannelReady = false;
const int BLE_MTU_NEGOTIATION_DELAY_MS = 2000;
const int BLE_SERVICE_DISCOVERY_RETRIES = 5;
const int BLE_SERVICE_DISCOVERY_RETRY_DELAY_MS = 1000;
const int BLE_DEINIT_CLEANUP_DELAY_MS = 100; // Allow BLE stack to complete cleanup before deinit
const int BLE_CLIENT_CLEANUP_DELAY_MS = 100; // Allow BLE stack to complete cleanup when recreating client
const int BLE_NOTIFY_REGISTRATION_DELAY_MS = 500; // Allow CCCD write to complete after registering for notifications

// BLE/WiFi coexistence - ESP32-S3 cannot run WiFi and BLE simultaneously
bool bleOperationInProgress = false;
bool monitoringPaused = false;
bool bleInitialized = false;

// MeshCore protocol state
enum MeshCoreState {
  MESH_STATE_DISCONNECTED = 0,
  MESH_STATE_CONNECTED,
  MESH_STATE_NEGOTIATED,  // After CMD_DEVICE_QUERY
  MESH_STATE_READY        // After CMD_APP_START
};
MeshCoreState meshProtocolState = MESH_STATE_DISCONNECTED;
uint8_t meshProtocolVersion = 3;  // Target protocol version
uint8_t meshChannelIndex = 0;     // Channel index for sending messages

// Nordic UART Service UUIDs (used by MeshCore BLE companion firmware)
const char* NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const char* NUS_TX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // Write to this
const char* NUS_RX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // Notifications from this

// MeshCore Companion Radio Protocol Commands
const uint8_t CMD_APP_START = 1;
const uint8_t CMD_SEND_CHANNEL_TXT_MSG = 3;
const uint8_t CMD_GET_CHANNEL = 31;   // 0x1F
const uint8_t CMD_SET_CHANNEL = 32;   // 0x20
const uint8_t CMD_DEVICE_QUERY = 22;  // 0x16

// MeshCore Protocol Response Codes
const uint8_t RESP_CODE_OK = 0;
const uint8_t RESP_CODE_ERR = 1;
const uint8_t RESP_CODE_SELF_INFO = 5;
const uint8_t RESP_CODE_SENT = 6;
const uint8_t RESP_CODE_DEVICE_INFO = 13;
const uint8_t RESP_CODE_CHANNEL_INFO = 18;

// MeshCore Protocol Error Codes
const uint8_t ERR_CODE_NOT_FOUND = 2;

// Maximum frame payload size (MeshCore limit is 172 bytes, BLE MTU is typically 185-512 after negotiation)
const uint16_t MAX_FRAME_PAYLOAD = 172;

// Note: For BLE, frames are sent/received as raw payloads - no framing markers needed.
// The USB protocol uses '<' and '>' markers with length prefix, but BLE doesn't need this
// because each BLE characteristic write/notification is a complete frame.

// MeshCore channel configuration sizes
const size_t MESH_CHANNEL_NAME_SIZE = 32;
const size_t MESH_CHANNEL_KEY_SIZE = 16;

// Reserved bytes in CMD_APP_START
const size_t APP_START_RESERVED_SIZE = 6;

// Maximum text message length (conservative for LoRa payload)
const size_t MAX_TEXT_MESSAGE_LEN = 140;

// RX buffer for storing received BLE notification payload
const int MESH_RX_BUFFER_SIZE = 256;
uint8_t meshRxBuffer[MESH_RX_BUFFER_SIZE];
int meshRxPayloadLen = 0;  // Length of current payload in buffer
volatile bool meshResponseReceived = false;
uint8_t meshLastResponseCode = 0xFF;

class MeshClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* client) override {
    meshDeviceConnected = true;
    lastMeshError = "";
  }

  void onDisconnect(BLEClient* client) override {
    meshDeviceConnected = false;
    meshChannelReady = false;
    meshProtocolState = MESH_STATE_DISCONNECTED;
  }
};

// Static callback instance to avoid memory leak from repeated allocations
static MeshClientCallbacks meshClientCallbacks;

// Notification callback for receiving data from MeshCore device
// For BLE: each notification is a complete frame (no framing markers needed)
// The first byte of the payload is the response/push code
void meshNotifyCallback(BLERemoteCharacteristic* pCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  // For BLE, each notification is a complete frame - no accumulation needed
  // The BLE link layer ensures frame integrity
  
  if (length == 0) {
    Serial.println("MeshCore RX: empty notification received");
    return;
  }
  
  // Reject oversized payloads to prevent buffer overflow and data corruption
  if (length > MESH_RX_BUFFER_SIZE) {
    Serial.printf("MeshCore RX: payload too large (%d > %d), rejecting frame\n", 
                  (int)length, MESH_RX_BUFFER_SIZE);
    meshRxPayloadLen = 0;
    meshResponseReceived = false;
    return;
  }
  
  // First byte is the response code
  meshLastResponseCode = pData[0];
  Serial.printf("MeshCore RX: response code 0x%02X, length %d\n", meshLastResponseCode, (int)length);
  
  // Store the complete payload in the buffer for any code that might need to parse additional data
  memcpy(meshRxBuffer, pData, length);
  meshRxPayloadLen = length;
  
  meshResponseReceived = true;
}

// Send a frame to the MeshCore device
// For BLE: frames are sent as raw payload (no framing needed - BLE handles integrity)
// For USB: frames would need '<' + len_lo + len_hi + payload framing (not used here)
bool sendMeshFrame(const uint8_t* payload, size_t payloadLen) {
  if (!meshDeviceConnected || meshTxCharacteristic == nullptr || meshProtocolState < MESH_STATE_CONNECTED) {
    return false;
  }
  
  // Validate payload length
  if (payloadLen > MAX_FRAME_PAYLOAD) {
    Serial.printf("MeshCore TX: payload too large (%d > %d)\n", (int)payloadLen, MAX_FRAME_PAYLOAD);
    return false;
  }
  
  Serial.printf("MeshCore TX: cmd 0x%02X, payload length %d\n", payload[0], (int)payloadLen);
  
  bool success = false;
  try {
    // Reset response state before sending
    meshResponseReceived = false;
    meshLastResponseCode = 0xFF;
    
    // For BLE, write the raw payload directly - no framing needed
    // The BLE link layer handles frame integrity
    meshTxCharacteristic->writeValue(const_cast<uint8_t*>(payload), payloadLen, false);
    success = true;
  } catch (...) {
    Serial.println("MeshCore TX failed: write error");
  }
  
  return success;
}

// Wait for a response from the MeshCore device
bool waitForMeshResponse(unsigned long timeoutMs = 5000) {
  unsigned long start = millis();
  while (!meshResponseReceived && (millis() - start) < timeoutMs) {
    delay(10);
  }
  return meshResponseReceived;
}

// Service types
// Right now the behavior for each is rudimentary
// However, you can use this to expand and add services with more complex checks
enum ServiceType {
  TYPE_HOME_ASSISTANT,
  TYPE_JELLYFIN,
  TYPE_HTTP_GET,
  TYPE_PING
};

// Service structure
struct Service {
  String id;
  String name;
  ServiceType type;
  String host;
  int port;
  String path;
  String expectedResponse;
  int checkInterval;
  int passThreshold;      // Number of consecutive passes required to mark as UP
  int failThreshold;      // Number of consecutive fails required to mark as DOWN
  int consecutivePasses;  // Current count of consecutive passes
  int consecutiveFails;   // Current count of consecutive fails
  bool isUp;
  unsigned long lastCheck;
  unsigned long lastUptime;
  String lastError;
  int secondsSinceLastCheck;
};

// Store up to 20 services
const int MAX_SERVICES = 20;
Service services[MAX_SERVICES];
int serviceCount = 0;

// prototype declarations
void initWiFi();
void initWebServer();
void initFileSystem();
bool ensureAuthenticated(AsyncWebServerRequest* request);
void loadServices();
void saveServices();
String generateServiceId();
void checkServices();
void sendOfflineNotification(const Service& service);
void sendOnlineNotification(const Service& service);
void sendSmtpNotification(const String& title, const String& message);
bool checkHomeAssistant(Service& service);
bool checkJellyfin(Service& service);
bool checkHttpGet(Service& service);
bool checkPing(Service& service);
String getWebPage();
String getServiceTypeString(ServiceType type);
String base64Encode(const String& input);
bool readSmtpResponse(WiFiClient& client, int expectedCode);
bool sendSmtpCommand(WiFiClient& client, const String& command, int expectedCode);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting ESP32 Uptime Monitor...");

  // Initialize filesystem
  initFileSystem();

  // Scan for BLE devices at boot and log them to serial monitor
  // This runs before WiFi to avoid ESP32-S3 WiFi/BLE coexistence issues
  scanBLEDevices();

  // Initialize WiFi (BLE is deinitialized after scan to allow this)
  initWiFi();

  // Load saved services
  loadServices();

  // Initialize web server
  initWebServer();

  Serial.println("System ready!");
  Serial.print("Access web interface at: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  static unsigned long lastCheckTime = 0;
  unsigned long currentTime = millis();

  // Skip service checks if monitoring is paused (e.g., during BLE operations)
  if (monitoringPaused) {
    delay(10);
    return;
  }

  // Check services every 5 seconds
  if (currentTime - lastCheckTime >= 5000) {
    checkServices();
    lastCheckTime = currentTime;
  }

  delay(10);
}

void initWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi!");
  }
}

void initBLE() {
  Serial.println("Initializing BLE MeshCore client...");
  Serial.printf("Free heap before BLE init: %d bytes\n", ESP.getFreeHeap());
  
  BLEDevice::init(BLE_DEVICE_NAME);
  
  // Verify initialization succeeded before continuing
  if (!BLEDevice::getInitialized()) {
    Serial.println("ERROR: BLE initialization failed - check if Bluetooth is enabled in sdkconfig");
    Serial.println("Common causes: insufficient memory, Bluetooth disabled in build configuration, or hardware issue");
    Serial.printf("Free heap after failed init: %d bytes\n", ESP.getFreeHeap());
    return;
  }
  bleInitialized = true;
  Serial.printf("BLE initialized successfully as '%s'\n", BLE_DEVICE_NAME);
  Serial.printf("Free heap after BLE init: %d bytes\n", ESP.getFreeHeap());

  // Set up PIN-based pairing to satisfy MeshCore's security expectations
  BLESecurity* security = new BLESecurity();
  security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security->setCapability(ESP_IO_CAP_OUT);
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setStaticPIN(BLE_PAIRING_PIN);

  connectToMeshCore();
}

void scanBLEDevices() {
  Serial.println("========================================");
  Serial.println("Starting BLE device scan...");
  Serial.println("========================================");
  
  // Log available heap before BLE initialization for debugging memory issues
  Serial.printf("Free heap before BLE init: %d bytes\n", ESP.getFreeHeap());
  
  // Initialize BLE for scanning
  BLEDevice::init(BLE_DEVICE_NAME);
  
  if (!BLEDevice::getInitialized()) {
    Serial.println("ERROR: BLE initialization failed, cannot scan for devices");
    Serial.println("Possible causes:");
    Serial.println("  - Bluetooth disabled in build configuration (CONFIG_BT_ENABLED)");
    Serial.println("  - Insufficient heap memory for BLE stack");
    Serial.println("  - Hardware issue with the ESP32 Bluetooth radio");
    Serial.printf("Free heap after failed init: %d bytes\n", ESP.getFreeHeap());
    return;
  }
  
  Serial.printf("BLE initialized successfully as '%s'\n", BLE_DEVICE_NAME);
  Serial.printf("Free heap after BLE init: %d bytes\n", ESP.getFreeHeap());
  
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
    BLEDevice::deinit();
    return;
  }
  
  Serial.println("========================================");
  Serial.printf("BLE Scan Complete: %d device(s) found\n", deviceCount);
  Serial.println("========================================");
  
  for (int i = 0; i < deviceCount; i++) {
    // make a copy of the advertised device (don't bind to a reference to an internal/temporary object)
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
    
    if (device.haveManufacturerData()) {
      Serial.println("    Manufacturer data: present");
    }
    
    Serial.println("----------------------------------------");
  }
  
  // Clean up scan results before deinitializing BLE
  scan->clearResults();
  
  // Deinitialize BLE so WiFi can be used (ESP32-S3 cannot run both simultaneously)
  BLEDevice::deinit();
  
  Serial.println("========================================");
  Serial.println("BLE scan complete, BLE deinitialized for WiFi usage");
  Serial.println("NOTE: BLE will be re-initialized on-demand when sending MeshCore notifications");
  Serial.println("========================================");
}

bool ensureAuthenticated(AsyncWebServerRequest* request) {
  if (strlen(WEB_AUTH_USERNAME) == 0 || strlen(WEB_AUTH_PASSWORD) == 0) {
    return true;
  }

  if (request->authenticate(WEB_AUTH_USERNAME, WEB_AUTH_PASSWORD)) {
    return true;
  }

  request->requestAuthentication();
  return false;
}

void initFileSystem() {
  // First attempt: try to mount without formatting
  if (LittleFS.begin(false)) {
    Serial.println("LittleFS mounted successfully");
    return;
  }

  Serial.println("LittleFS mount failed, attempting format...");

  // If mount fails, explicitly format the filesystem first
  // This handles severely corrupted filesystems better than begin(true)
  if (!LittleFS.format()) {
    Serial.println("LittleFS format failed! Check partition table and flash configuration.");
    return;
  }

  Serial.println("LittleFS formatted successfully");

  // Now try to mount the freshly formatted filesystem
  if (!LittleFS.begin(false)) {
    Serial.println("Critical: Failed to mount LittleFS after successful format!");
    return;
  }

  Serial.println("LittleFS mounted successfully after format");
}

bool connectToMeshCore() {
  lastMeshConnectAttempt = millis();
  meshDeviceConnected = false;
  meshTxCharacteristic = nullptr;
  meshRxCharacteristic = nullptr;
  meshChannelReady = false;
  meshProtocolState = MESH_STATE_DISCONNECTED;
  meshRxPayloadLen = 0;

  Serial.printf("Scanning for MeshCore peer named '%s' (extended debug)...\n", BLE_PEER_NAME);

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  // Tighten scan timing to improve chances of catching short MeshCore adverts
  scan->setInterval(200);
  scan->setWindow(160);

  const int scanSeconds = 10;
  BLEScanResults results = scan->start(scanSeconds, false);

  Serial.printf("Scan complete: %d devices found\n", results.getCount());

  for (int i = 0; i < results.getCount(); i++) {
    BLEAdvertisedDevice device = results.getDevice(i);

    // Debug: print what we saw for each advertiser
    std::string rawName = device.getName();
    String devName = rawName.length() ? String(rawName.c_str()) : String("");
    String devAddr = String(device.getAddress().toString().c_str());
    int rssi = device.getRSSI();
    Serial.printf("  [%d] Name='%s' Addr=%s RSSI=%d\n", i, devName.c_str(), devAddr.c_str(), rssi);

    // Print any advertised single service UUID (if present)
    bool svcPrinted = false;
    if (device.haveServiceUUID()) {
      BLEUUID adv = device.getServiceUUID();
      Serial.printf("       Advertised service UUID: %s\n", adv.toString().c_str());
      svcPrinted = true;
    }
    if (!svcPrinted) {
      Serial.println("       No single advertised service UUID available");
    }

    // Matching logic: exact name, substring, or advertised Nordic UART service UUID match
    bool nameMatches = (devName.length() && devName == String(BLE_PEER_NAME));
    bool nameContains = (devName.length() && devName.indexOf(String(BLE_PEER_NAME)) >= 0);
    bool serviceMatches = false;

    if (device.haveServiceUUID()) {
      BLEUUID adv = device.getServiceUUID();
      // Compare to Nordic UART Service UUID
      serviceMatches = adv.equals(BLEUUID(NUS_SERVICE_UUID));
    }

    if (!(nameMatches || nameContains || serviceMatches)) {
      Serial.println("       Not the MeshCore peer (name/service mismatch), skipping.");
      continue;
    }

    Serial.println("MeshCore peer candidate found, attempting connection...");

    // Clean up any existing client before creating a new one to avoid "Deregister Failed" errors
    if (meshClient != nullptr) {
      if (meshClient->isConnected()) {
        meshClient->disconnect();
      }
      delete meshClient;
      meshClient = nullptr;
      delay(BLE_CLIENT_CLEANUP_DELAY_MS);
    }

    meshClient = BLEDevice::createClient();
    meshClient->setClientCallbacks(&meshClientCallbacks);

    // Copy the address so we don't rely on the temporary BLEAdvertisedDevice
    BLEAddress peerAddress = device.getAddress();

    // Determine address type from the advertised device
    // MeshCore devices typically use random addresses
    esp_ble_addr_type_t addrType = device.getAddressType();
    
    if (!meshClient->connect(peerAddress, addrType)) {
      lastMeshError = "Connection attempt failed";
      Serial.println("Connection attempt failed");
      continue;
    }

    // Allow MTU negotiation and security handshake to complete before accessing services
    delay(BLE_MTU_NEGOTIATION_DELAY_MS);

    // Retry service discovery to handle cases where MTU negotiation may still be completing
    // Try Nordic UART Service (NUS) - used by MeshCore BLE companion firmware
    BLERemoteService* service = nullptr;
    std::map<std::string, BLERemoteService*>* discoveredServices = nullptr;
    for (int retry = 0; retry < BLE_SERVICE_DISCOVERY_RETRIES; retry++) {
      service = meshClient->getService(NUS_SERVICE_UUID);
      if (service != nullptr) {
        Serial.printf("Found MeshCore service on attempt %d\n", retry + 1);
        break;
      }
      
      // If direct lookup fails, try full service discovery and look for our service
      discoveredServices = meshClient->getServices();
      if (discoveredServices != nullptr && !discoveredServices->empty()) {
        Serial.printf("Service discovery attempt %d: found %d service(s):\n", retry + 1, discoveredServices->size());
        for (auto& svc : *discoveredServices) {
          Serial.printf("  - Service UUID: %s\n", svc.first.c_str());
          // Check if this matches our target service UUID
          if (BLEUUID(svc.first).equals(BLEUUID(NUS_SERVICE_UUID))) {
            service = svc.second;
            Serial.println("  ^ Found matching MeshCore service!");
            break;
          }
        }
        if (service != nullptr) {
          break;
        }
      } else {
        Serial.printf("Service discovery attempt %d: no services found yet\n", retry + 1);
      }
      
      if (retry < BLE_SERVICE_DISCOVERY_RETRIES - 1) {
        Serial.printf("Retrying service discovery in %d ms...\n", BLE_SERVICE_DISCOVERY_RETRY_DELAY_MS);
        delay(BLE_SERVICE_DISCOVERY_RETRY_DELAY_MS);
      }
    }
    if (service == nullptr) {
      lastMeshError = "Nordic UART Service not found on peer";
      Serial.println("Nordic UART Service not found on peer, disconnecting...");
      meshClient->disconnect();
      continue;
    }

    // Get TX characteristic (ESP32 writes to this)
    BLERemoteCharacteristic* txChar = service->getCharacteristic(NUS_TX_CHAR_UUID);
    if (txChar == nullptr) {
      lastMeshError = "NUS TX characteristic missing";
      Serial.println("NUS TX characteristic missing, disconnecting...");
      meshClient->disconnect();
      continue;
    }

    if (!txChar->canWrite()) {
      lastMeshError = "NUS TX characteristic is not writable";
      Serial.println("NUS TX characteristic is not writable, disconnecting...");
      meshClient->disconnect();
      continue;
    }

    // Get RX characteristic (ESP32 receives notifications from this)
    BLERemoteCharacteristic* rxChar = service->getCharacteristic(NUS_RX_CHAR_UUID);
    if (rxChar == nullptr) {
      lastMeshError = "NUS RX characteristic missing";
      Serial.println("NUS RX characteristic missing, disconnecting...");
      meshClient->disconnect();
      continue;
    }

    if (!rxChar->canNotify()) {
      lastMeshError = "NUS RX characteristic cannot notify";
      Serial.println("NUS RX characteristic cannot notify, disconnecting...");
      meshClient->disconnect();
      continue;
    }

    // Register for notifications
    rxChar->registerForNotify(meshNotifyCallback);
    
    // Wait for notification subscription to be established (CCCD write to complete)
    delay(BLE_NOTIFY_REGISTRATION_DELAY_MS);
    
    meshTxCharacteristic = txChar;
    meshRxCharacteristic = rxChar;
    meshDeviceConnected = true;
    meshProtocolState = MESH_STATE_CONNECTED;
    lastMeshError = "";
    Serial.println("Connected to MeshCore peer via Nordic UART Service");

    // Perform MeshCore protocol handshake
    if (!ensureMeshChannel()) {
      Serial.println("ensureMeshChannel() failed, disconnecting...");
      meshClient->disconnect();
      meshDeviceConnected = false;
      meshProtocolState = MESH_STATE_DISCONNECTED;
      return false;
    }

    return true;
  }

  // Free scan results so the BLE stack can reuse memory on the next attempt
  scan->clearResults();

  lastMeshError = "MeshCore peer not found during scan";
  Serial.println(lastMeshError.c_str());
  return false;
}
void disconnectFromMeshCore() {
  // Clean up the BLE client properly before deinit
  if (meshClient != nullptr) {
    if (meshClient->isConnected()) {
      Serial.println("Disconnecting from MeshCore...");
      meshClient->disconnect();
    }
    // Delete the client to properly deregister it from BLE stack
    delete meshClient;
    meshClient = nullptr;
  }
  
  meshDeviceConnected = false;
  meshTxCharacteristic = nullptr;
  meshRxCharacteristic = nullptr;
  meshChannelReady = false;
  meshProtocolState = MESH_STATE_DISCONNECTED;
  meshRxPayloadLen = 0;
  
  // Only deinitialize BLE if it was initialized
  if (bleInitialized) {
    delay(BLE_DEINIT_CLEANUP_DELAY_MS);
    BLEDevice::deinit();
    bleInitialized = false;
    Serial.println("BLE deinitialized");
  }
}

void disconnectWiFi() {
  Serial.println("Disconnecting WiFi for BLE operation...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100); // Allow WiFi to fully shut down
  Serial.println("WiFi disconnected");
}

void reconnectWiFi() {
  Serial.println("Reconnecting WiFi after BLE operation...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi reconnected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to reconnect to WiFi!");
  }
}

bool ensureMeshChannel() {
  if (meshChannelReady && meshProtocolState == MESH_STATE_READY) {
    return true;
  }

  if (!meshDeviceConnected || meshTxCharacteristic == nullptr) {
    return false;
  }

  // Step 1: Send CMD_DEVICE_QUERY to negotiate protocol version
  if (meshProtocolState < MESH_STATE_NEGOTIATED) {
    Serial.println("Sending CMD_DEVICE_QUERY...");
    uint8_t queryCmd[2] = { CMD_DEVICE_QUERY, meshProtocolVersion };
    if (!sendMeshFrame(queryCmd, sizeof(queryCmd))) {
      lastMeshError = "Failed to send CMD_DEVICE_QUERY";
      return false;
    }
    
    if (!waitForMeshResponse(5000)) {
      lastMeshError = "Timeout waiting for RESP_CODE_DEVICE_INFO";
      return false;
    }
    
    if (meshLastResponseCode != RESP_CODE_DEVICE_INFO) {
      lastMeshError = "Unexpected response to CMD_DEVICE_QUERY";
      Serial.printf("Expected RESP_CODE_DEVICE_INFO (0x%02X), got 0x%02X\n", RESP_CODE_DEVICE_INFO, meshLastResponseCode);
      return false;
    }
    
    meshProtocolState = MESH_STATE_NEGOTIATED;
    Serial.println("Protocol negotiated successfully");
  }

  // Step 2: Send CMD_APP_START to start the application session
  if (meshProtocolState < MESH_STATE_READY) {
    Serial.println("Sending CMD_APP_START...");
    // CMD_APP_START payload: version (1), reserved (6), app_name (variable)
    const char* appName = "ESP32-Uptime";
    size_t appNameLen = strlen(appName);
    size_t cmdLen = 1 + 1 + APP_START_RESERVED_SIZE + appNameLen;
    uint8_t* startCmd = new uint8_t[cmdLen];
    startCmd[0] = CMD_APP_START;
    startCmd[1] = meshProtocolVersion;  // version
    memset(startCmd + 2, 0, APP_START_RESERVED_SIZE);  // reserved bytes
    memcpy(startCmd + 2 + APP_START_RESERVED_SIZE, appName, appNameLen);
    
    bool sent = sendMeshFrame(startCmd, cmdLen);
    delete[] startCmd;
    
    if (!sent) {
      lastMeshError = "Failed to send CMD_APP_START";
      return false;
    }
    
    if (!waitForMeshResponse(5000)) {
      lastMeshError = "Timeout waiting for RESP_CODE_SELF_INFO";
      return false;
    }
    
    if (meshLastResponseCode != RESP_CODE_SELF_INFO) {
      lastMeshError = "Unexpected response to CMD_APP_START";
      Serial.printf("Expected RESP_CODE_SELF_INFO (0x%02X), got 0x%02X\n", RESP_CODE_SELF_INFO, meshLastResponseCode);
      return false;
    }
    
    meshProtocolState = MESH_STATE_READY;
    Serial.println("Application session started successfully");
  }

  // Step 3: Provision the channel if needed
  if (!provisionMeshChannel()) {
    lastMeshError = "Mesh channel provisioning failed";
    return false;
  }

  meshChannelReady = true;
  return true;
}

bool provisionMeshChannel() {
  if (strlen(BLE_MESH_CHANNEL_NAME) == 0) {
    lastMeshError = "Mesh channel name missing";
    return false;
  }

  Serial.printf("Checking/provisioning MeshCore channel '%s'...\n", BLE_MESH_CHANNEL_NAME);
  
  // First try to get the channel at index 0 to see if it exists
  uint8_t getChannelCmd[2] = { CMD_GET_CHANNEL, meshChannelIndex };
  if (!sendMeshFrame(getChannelCmd, sizeof(getChannelCmd))) {
    lastMeshError = "Failed to send CMD_GET_CHANNEL";
    return false;
  }
  
  if (!waitForMeshResponse(3000)) {
    Serial.println("No response to CMD_GET_CHANNEL, will try to set channel");
  } else if (meshLastResponseCode == RESP_CODE_CHANNEL_INFO) {
    // Channel exists, we can use it
    Serial.println("Channel already exists, ready to use");
    return true;
  }
  
  // Channel doesn't exist or error, try to create/set it
  // CMD_SET_CHANNEL payload: channel_index (1), name (32 bytes null-padded), key (16 bytes)
  Serial.printf("Setting channel at index %d...\n", meshChannelIndex);
  
  uint8_t setChannelCmd[1 + 1 + MESH_CHANNEL_NAME_SIZE + MESH_CHANNEL_KEY_SIZE];
  memset(setChannelCmd, 0, sizeof(setChannelCmd));
  setChannelCmd[0] = CMD_SET_CHANNEL;
  setChannelCmd[1] = meshChannelIndex;
  
  // Copy channel name (up to MESH_CHANNEL_NAME_SIZE bytes, null-padded)
  size_t nameLen = strlen(BLE_MESH_CHANNEL_NAME);
  if (nameLen > MESH_CHANNEL_NAME_SIZE) nameLen = MESH_CHANNEL_NAME_SIZE;
  memcpy(setChannelCmd + 2, BLE_MESH_CHANNEL_NAME, nameLen);
  
  // Copy channel key (up to MESH_CHANNEL_KEY_SIZE bytes, null-padded)
  size_t keyLen = strlen(BLE_MESH_CHANNEL_KEY);
  if (keyLen > MESH_CHANNEL_KEY_SIZE) keyLen = MESH_CHANNEL_KEY_SIZE;
  memcpy(setChannelCmd + 2 + MESH_CHANNEL_NAME_SIZE, BLE_MESH_CHANNEL_KEY, keyLen);
  
  if (!sendMeshFrame(setChannelCmd, sizeof(setChannelCmd))) {
    lastMeshError = "Failed to send CMD_SET_CHANNEL";
    return false;
  }
  
  if (!waitForMeshResponse(3000)) {
    lastMeshError = "Timeout waiting for channel set response";
    return false;
  }
  
  if (meshLastResponseCode == RESP_CODE_OK) {
    Serial.println("Channel set successfully");
    lastMeshError = "";
    return true;
  } else if (meshLastResponseCode == RESP_CODE_ERR) {
    lastMeshError = "Failed to set channel (device error)";
    return false;
  }
  
  // Any other response - assume it worked
  Serial.printf("Received response code 0x%02X, assuming success\n", meshLastResponseCode);
  lastMeshError = "";
  return true;
}

void initWebServer() {

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!ensureAuthenticated(request)) {
      return;
    }
    request->send(200, "text/html", getWebPage());
  });

  server.on("/api/mesh/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["connected"] = meshDeviceConnected;
    doc["peerName"] = BLE_PEER_NAME;
    doc["deviceName"] = BLE_DEVICE_NAME;
    doc["channel"] = BLE_MESH_CHANNEL_NAME;
    doc["channelReady"] = meshChannelReady;
    doc["protocolState"] = (int)meshProtocolState;
    doc["lastError"] = lastMeshError;
    doc["bleOperationInProgress"] = bleOperationInProgress;
    doc["monitoringPaused"] = monitoringPaused;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/api/mesh/send", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!ensureAuthenticated(request)) {
        return;
      }

      // Check if a BLE operation is already in progress
      if (bleOperationInProgress) {
        request->send(503, "application/json", "{\"error\":\"BLE operation already in progress\"}");
        return;
      }

      JsonDocument doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      String title = doc["title"] | "Mesh Message";
      String message = doc["message"] | "";
      if (message.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"Missing message\"}");
        return;
      }

      // Note: sendMeshCoreNotification handles WiFi/BLE switching internally
      // The response is sent before the operation completes because the async
      // web server would timeout during the WiFi/BLE switch.
      // The actual sending happens synchronously after the response.
      request->send(202, "application/json", "{\"success\":true,\"status\":\"queued\"}");
      
      // Send the notification (this blocks while doing WiFi/BLE switch)
      sendMeshCoreNotification(title, message);
    }
  );

  // get services
  server.on("/api/services", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray array = doc["services"].to<JsonArray>();

    unsigned long currentTime = millis();

    for (int i = 0; i < serviceCount; i++) {
      if (services[i].lastCheck > 0) {
        services[i].secondsSinceLastCheck = (currentTime - services[i].lastCheck) / 1000;
      } else {
        services[i].secondsSinceLastCheck = -1; // Never checked
      }

      JsonObject obj = array.add<JsonObject>();
      obj["id"] = services[i].id;
      obj["name"] = services[i].name;
      obj["type"] = getServiceTypeString(services[i].type);
      obj["host"] = services[i].host;
      obj["port"] = services[i].port;
      obj["path"] = services[i].path;
      obj["expectedResponse"] = services[i].expectedResponse;
      obj["checkInterval"] = services[i].checkInterval;
      obj["passThreshold"] = services[i].passThreshold;
      obj["failThreshold"] = services[i].failThreshold;
      obj["consecutivePasses"] = services[i].consecutivePasses;
      obj["consecutiveFails"] = services[i].consecutiveFails;
      obj["isUp"] = services[i].isUp;
      obj["secondsSinceLastCheck"] = services[i].secondsSinceLastCheck;
      obj["lastError"] = services[i].lastError;
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // add service
  server.on("/api/services", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!ensureAuthenticated(request)) {
        return;
      }
      if (serviceCount >= MAX_SERVICES) {
        request->send(400, "application/json", "{\"error\":\"Maximum services reached\"}");
        return;
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      Service newService;
      newService.id = generateServiceId();
      newService.name = doc["name"].as<String>();

      String typeStr = doc["type"].as<String>();
      if (typeStr == "home_assistant") {
        newService.type = TYPE_HOME_ASSISTANT;
      } else if (typeStr == "jellyfin") {
        newService.type = TYPE_JELLYFIN;
      } else if (typeStr == "http_get") {
        newService.type = TYPE_HTTP_GET;
      } else if (typeStr == "ping") {
        newService.type = TYPE_PING;
      } else {
        request->send(400, "application/json", "{\"error\":\"Invalid service type\"}");
        return;
      }

      newService.host = doc["host"].as<String>();
      newService.port = doc["port"] | 80;
      newService.path = doc["path"] | "/";
      newService.expectedResponse = doc["expectedResponse"] | "*";
      newService.checkInterval = doc["checkInterval"] | 60;

      int passThreshold = doc["passThreshold"] | 1;
      if (passThreshold < 1) passThreshold = 1;
      newService.passThreshold = passThreshold;

      int failThreshold = doc["failThreshold"] | 1;
      if (failThreshold < 1) failThreshold = 1;
      newService.failThreshold = failThreshold;

      newService.consecutivePasses = 0;
      newService.consecutiveFails = 0;
      newService.isUp = false;
      newService.lastCheck = 0;
      newService.lastUptime = 0;
      newService.lastError = "";
      newService.secondsSinceLastCheck = -1;

      services[serviceCount++] = newService;
      saveServices();

      JsonDocument response;
      response["success"] = true;
      response["id"] = newService.id;

      String responseStr;
      serializeJson(response, responseStr);
      request->send(200, "application/json", responseStr);
    }
  );

  // delete service
  server.on("/api/services/*", HTTP_DELETE, [](AsyncWebServerRequest *request) {
    if (!ensureAuthenticated(request)) {
      return;
    }
    String path = request->url();
    String serviceId = path.substring(path.lastIndexOf('/') + 1);

    int foundIndex = -1;
    for (int i = 0; i < serviceCount; i++) {
      if (services[i].id == serviceId) {
        foundIndex = i;
        break;
      }
    }

    if (foundIndex == -1) {
      request->send(404, "application/json", "{\"error\":\"Service not found\"}");
      return;
    }

    // Shift services array
    for (int i = foundIndex; i < serviceCount - 1; i++) {
      services[i] = services[i + 1];
    }
    serviceCount--;

    saveServices();
    request->send(200, "application/json", "{\"success\":true}");
  });

  // export services configuration
  server.on("/api/export", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray array = doc["services"].to<JsonArray>();

    for (int i = 0; i < serviceCount; i++) {
      JsonObject obj = array.add<JsonObject>();
      obj["name"] = services[i].name;
      obj["type"] = getServiceTypeString(services[i].type);
      obj["host"] = services[i].host;
      obj["port"] = services[i].port;
      obj["path"] = services[i].path;
      obj["expectedResponse"] = services[i].expectedResponse;
      obj["checkInterval"] = services[i].checkInterval;
      obj["passThreshold"] = services[i].passThreshold;
      obj["failThreshold"] = services[i].failThreshold;
    }

    String response;
    serializeJson(doc, response);

    AsyncWebServerResponse *res = request->beginResponse(200, "application/json", response);
    res->addHeader("Content-Disposition", "attachment; filename=\"monitors-backup.json\"");
    request->send(res);
  });

  // import services configuration
  server.on("/api/import", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (!ensureAuthenticated(request)) {
        return;
      }
      // Limit payload size to 16KB to prevent DoS
      if (total > 16384) {
        request->send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, data, len);

      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      JsonArray array = doc["services"];
      if (array.isNull()) {
        request->send(400, "application/json", "{\"error\":\"Missing services array\"}");
        return;
      }

      int importedCount = 0;
      int skippedCount = 0;

      for (JsonObject obj : array) {
        if (serviceCount >= MAX_SERVICES) {
          skippedCount++;
          continue;
        }

        // Validate required fields
        String name = obj["name"].as<String>();
        String host = obj["host"].as<String>();
        if (name.length() == 0 || host.length() == 0) {
          skippedCount++;
          continue;
        }

        String typeStr = obj["type"].as<String>();
        ServiceType type;
        if (typeStr == "home_assistant") {
          type = TYPE_HOME_ASSISTANT;
        } else if (typeStr == "jellyfin") {
          type = TYPE_JELLYFIN;
        } else if (typeStr == "http_get") {
          type = TYPE_HTTP_GET;
        } else if (typeStr == "ping") {
          type = TYPE_PING;
        } else {
          skippedCount++;
          continue;
        }

        // Validate and constrain numeric values
        int port = obj["port"] | 80;
        if (port < 1 || port > 65535) port = 80;

        int checkInterval = obj["checkInterval"] | 60;
        if (checkInterval < 10) checkInterval = 10;

        int passThreshold = obj["passThreshold"] | 1;
        if (passThreshold < 1) passThreshold = 1;

        int failThreshold = obj["failThreshold"] | 1;
        if (failThreshold < 1) failThreshold = 1;

        Service newService;
        newService.id = generateServiceId();
        newService.name = name;
        newService.type = type;
        newService.host = host;
        newService.port = port;
        newService.path = obj["path"] | "/";
        newService.expectedResponse = obj["expectedResponse"] | "*";
        newService.checkInterval = checkInterval;
        newService.passThreshold = passThreshold;
        newService.failThreshold = failThreshold;
        newService.consecutivePasses = 0;
        newService.consecutiveFails = 0;
        newService.isUp = false;
        newService.lastCheck = 0;
        newService.lastUptime = 0;
        newService.lastError = "";
        newService.secondsSinceLastCheck = -1;

        services[serviceCount++] = newService;
        importedCount++;
      }

      saveServices();

      JsonDocument response;
      response["success"] = true;
      response["imported"] = importedCount;
      response["skipped"] = skippedCount;

      String responseStr;
      serializeJson(response, responseStr);
      request->send(200, "application/json", responseStr);
    }
  );

  server.begin();
  Serial.println("Web server started");
}

String generateServiceId() {
  return String(millis()) + String(random(1000, 9999));
}

void checkServices() {
  unsigned long currentTime = millis();

  for (int i = 0; i < serviceCount; i++) {
    // Check if it's time to check this service
    if (currentTime - services[i].lastCheck < services[i].checkInterval * 1000) {
      continue;
    }

    bool firstCheck = services[i].lastCheck == 0;
    services[i].lastCheck = currentTime;
    bool wasUp = services[i].isUp;

    // Perform the actual check
    bool checkResult = false;
    switch (services[i].type) {
      case TYPE_HOME_ASSISTANT:
        checkResult = checkHomeAssistant(services[i]);
        break;
      case TYPE_JELLYFIN:
        checkResult = checkJellyfin(services[i]);
        break;
      case TYPE_HTTP_GET:
        checkResult = checkHttpGet(services[i]);
        break;
      case TYPE_PING:
        checkResult = checkPing(services[i]);
        break;
    }

    // Update consecutive counters based on check result
    if (checkResult) {
      services[i].consecutivePasses++;
      services[i].consecutiveFails = 0;
      services[i].lastUptime = currentTime;
      services[i].lastError = "";
    } else {
      services[i].consecutiveFails++;
      services[i].consecutivePasses = 0;
    }

    // Determine new state based on thresholds
    if (!services[i].isUp && services[i].consecutivePasses >= services[i].passThreshold) {
      // Service has passed enough times to be considered UP
      services[i].isUp = true;
    } else if (services[i].isUp && services[i].consecutiveFails >= services[i].failThreshold) {
      // Service has failed enough times to be considered DOWN
      services[i].isUp = false;
    }

    // Log and notify on state changes
    if (wasUp != services[i].isUp) {
      Serial.printf("Service '%s' is now %s (after %d consecutive %s)\n",
        services[i].name.c_str(),
        services[i].isUp ? "UP" : "DOWN",
        services[i].isUp ? services[i].consecutivePasses : services[i].consecutiveFails,
        services[i].isUp ? "passes" : "fails");

      if (!services[i].isUp) {
        sendOfflineNotification(services[i]);
      } else if (!firstCheck) {
        sendOnlineNotification(services[i]);
      }
    }
  }
}

// technically just detectes any endpoint, so would be good to support auth and check if it's actually home assistant
// could parse /api/states or something to check there are valid entities and that it's actually HA
bool checkHomeAssistant(Service& service) {
  HTTPClient http;
  String url = "http://" + service.host + ":" + String(service.port) + "/api/";

  http.begin(url);
  http.setTimeout(5000);

  int httpCode = http.GET();
  bool isUp = false;

  if (httpCode > 0) {
      // HA returns 404 for /api/, but ANY positive HTTP status means the service is alive
      isUp = true;
  } else {
      service.lastError = "Connection failed: " + String(httpCode);
  }

  http.end();
  return isUp;
}

bool checkJellyfin(Service& service) {
  HTTPClient http;
  String url = "http://" + service.host + ":" + String(service.port) + "/health";

  http.begin(url);
  http.setTimeout(5000);

  int httpCode = http.GET();
  bool isUp = false;

  if (httpCode > 0) {
    if (httpCode == 200) {
      isUp = true;
    }
  } else {
    service.lastError = "Connection failed: " + String(httpCode);
  }

  http.end();
  return isUp;
}

bool checkHttpGet(Service& service) {
  HTTPClient http;
  String url = "http://" + service.host + ":" + String(service.port) + service.path;

  http.begin(url);
  http.setTimeout(5000);

  int httpCode = http.GET();
  bool isUp = false;

  if (httpCode > 0) {
    if (httpCode == 200) {
      if (service.expectedResponse == "*") {
        isUp = true;
      } else {
        String payload = http.getString();
        isUp = payload.indexOf(service.expectedResponse) >= 0;
        if (!isUp) {
          service.lastError = "Response mismatch";
        }
      }
    } else {
      service.lastError = "HTTP " + String(httpCode);
    }
  } else {
    service.lastError = "Connection failed: " + String(httpCode);
  }

  http.end();
  return isUp;
}

bool checkPing(Service& service) {
  bool success = Ping.ping(service.host.c_str(), 3);
  if (!success) {
    service.lastError = "Ping timeout";
  }
  return success;
}

void sendOfflineNotification(const Service& service) {
  if (!isNtfyConfigured() && !isDiscordConfigured() && !isSmtpConfigured() && !isMeshCoreConfigured()) {
    return;
  }

  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  String title = "Service DOWN: " + service.name;
  String message = "Service '" + service.name + "' at " + service.host;
  if (service.port > 0) {
    message += ":" + String(service.port);
  }
  message += " is offline.";

  if (service.lastError.length() > 0) {
    message += " Error: " + service.lastError;
  }

  if (wifiConnected) {
    if (isNtfyConfigured()) {
      sendNtfyNotification(title, message, "warning,monitor");
    }

    if (isDiscordConfigured()) {
      sendDiscordNotification(title, message);
    }

    if (isSmtpConfigured()) {
      sendSmtpNotification(title, message);
    }
  } else {
    Serial.println("WiFi offline: skipping internet notifications");
  }

  sendMeshCoreNotification(title, message);
}

void sendOnlineNotification(const Service& service) {
  if (!isNtfyConfigured() && !isDiscordConfigured() && !isSmtpConfigured() && !isMeshCoreConfigured()) {
    return;
  }

  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  String title = "Service UP: " + service.name;
  String message = "Service '" + service.name + "' at " + service.host;
  if (service.port > 0) {
    message += ":" + String(service.port);
  }
  message += " is back online.";

  if (wifiConnected) {
    if (isNtfyConfigured()) {
      sendNtfyNotification(title, message, "ok,monitor");
    }

    if (isDiscordConfigured()) {
      sendDiscordNotification(title, message);
    }

    if (isSmtpConfigured()) {
      sendSmtpNotification(title, message);
    }
  } else {
    Serial.println("WiFi offline: skipping internet notifications");
  }

  sendMeshCoreNotification(title, message);
}

void sendNtfyNotification(const String& title, const String& message, const String& tags) {
  HTTPClient http;
  String url = String(NTFY_SERVER) + "/" + NTFY_TOPIC;

  WiFiClientSecure client;
  bool isSecure = url.startsWith("https://");
  if (isSecure) {
    client.setInsecure();
    http.begin(client, url);
  } else {
    http.begin(url);
  }
  http.addHeader("Title", title);
  http.addHeader("Tags", tags);
  http.addHeader("Content-Type", "text/plain");

  if (strlen(NTFY_ACCESS_TOKEN) > 0) {
    http.addHeader("Authorization", "Bearer " + String(NTFY_ACCESS_TOKEN));
  } else if (strlen(NTFY_USERNAME) > 0) {
    http.setAuthorization(NTFY_USERNAME, NTFY_PASSWORD);
  }

  int httpCode = http.POST(message);

  if (httpCode > 0) {
    Serial.printf("ntfy notification sent: %d\n", httpCode);
  } else {
    Serial.printf("Failed to send ntfy notification: %d\n", httpCode);
  }

  http.end();
}

void sendDiscordNotification(const String& title, const String& message) {
  HTTPClient http;
  String url = String(DISCORD_WEBHOOK_URL);

  WiFiClientSecure client;
  bool isSecure = url.startsWith("https://");
  if (isSecure) {
    client.setInsecure();
    http.begin(client, url);
  } else {
    http.begin(url);
  }

  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["content"] = "**" + title + "**\n" + message;

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    Serial.printf("Discord notification sent: %d\n", httpCode);
  } else {
    Serial.printf("Failed to send Discord notification: %d\n", httpCode);
  }

  http.end();
}

void sendMeshCoreNotification(const String& title, const String& message) {
  // ESP32-S3 cannot run WiFi and BLE simultaneously
  // Disconnect WiFi, connect BLE, send message, disconnect BLE, reconnect WiFi
  
  Serial.println("Starting MeshCore notification (BLE operation)...");
  
  // Pause monitoring to prevent false positives during network transition
  monitoringPaused = true;
  bleOperationInProgress = true;
  
  // Disconnect WiFi before starting BLE
  disconnectWiFi();
  
  // Initialize BLE (on-demand, since we deinit after each use)
  initBLE();
  
  // Try to connect and send message
  if (meshDeviceConnected && meshTxCharacteristic != nullptr) {
    if (ensureMeshChannel()) {
      // Build the message: combine title and message
      String fullMessage = title + ": " + message;
      
      // CMD_SEND_CHANNEL_TXT_MSG payload:
      // txt_type (1 byte) - 0 for plain text
      // channel_index (1 byte)
      // timestamp (4 bytes, little-endian, unix seconds)
      // text (variable, UTF-8)
      
      size_t textLen = fullMessage.length();
      if (textLen > MAX_TEXT_MESSAGE_LEN) textLen = MAX_TEXT_MESSAGE_LEN;
      
      // CMD_SEND_CHANNEL_TXT_MSG: cmd(1) + txt_type(1) + channel_index(1) + timestamp(4) + text
      const size_t TIMESTAMP_SIZE = 4;
      size_t cmdLen = 1 + 1 + 1 + TIMESTAMP_SIZE + textLen;
      uint8_t* sendCmd = new uint8_t[cmdLen];
      
      sendCmd[0] = CMD_SEND_CHANNEL_TXT_MSG;
      sendCmd[1] = 0;  // txt_type = TXT_TYPE_PLAIN
      sendCmd[2] = meshChannelIndex;
      
      // Timestamp - use 0 to indicate "now" to the device, which will use its own clock
      // The MeshCore device typically has a more accurate time source
      // When timestamp is 0, the device substitutes its current time
      uint32_t timestamp = 0;
      sendCmd[3] = timestamp & 0xFF;
      sendCmd[4] = (timestamp >> 8) & 0xFF;
      sendCmd[5] = (timestamp >> 16) & 0xFF;
      sendCmd[6] = (timestamp >> 24) & 0xFF;
      
      // Copy message text
      memcpy(sendCmd + 7, fullMessage.c_str(), textLen);
      
      if (sendMeshFrame(sendCmd, cmdLen)) {
        // Wait for send confirmation
        if (waitForMeshResponse(5000)) {
          if (meshLastResponseCode == RESP_CODE_OK || meshLastResponseCode == RESP_CODE_SENT) {
            Serial.println("MeshCore notification sent successfully");
          } else {
            Serial.printf("MeshCore notification: unexpected response 0x%02X\n", meshLastResponseCode);
          }
        } else {
          Serial.println("MeshCore notification: no response (may still be sent)");
        }
      } else {
        Serial.println("MeshCore notification failed: write error");
      }
      
      delete[] sendCmd;
    } else {
      Serial.println("MeshCore notification skipped: channel not ready");
    }
  } else {
    Serial.println("MeshCore notification skipped: not connected");
  }
  
  // Disconnect BLE and deinitialize to free resources
  disconnectFromMeshCore();
  
  // Reconnect WiFi
  reconnectWiFi();
  
  // Resume monitoring
  bleOperationInProgress = false;
  monitoringPaused = false;
  
  Serial.println("MeshCore notification operation complete");
}

String base64Encode(const String& input) {
  size_t outputLength = 0;
  size_t bufferLength = ((input.length() + 2) / 3) * 4 + 4;
  unsigned char* output = new unsigned char[bufferLength];

  int result = mbedtls_base64_encode(
    output,
    bufferLength,
    &outputLength,
    reinterpret_cast<const unsigned char*>(input.c_str()),
    input.length()
  );

  String encoded = "";
  if (result == 0) {
    encoded = String(reinterpret_cast<char*>(output), outputLength);
  }

  delete[] output;
  return encoded;
}

bool readSmtpResponse(WiFiClient& client, int expectedCode) {
  unsigned long timeout = millis() + 5000;
  String line;
  int code = -1;

  do {
    while (!client.available() && millis() < timeout) {
      delay(10);
    }

    if (!client.available()) {
      Serial.println("SMTP response timeout");
      return false;
    }

    line = client.readStringUntil('\n');
    line.trim();
    if (line.length() >= 3) {
      code = line.substring(0, 3).toInt();
    }
  } while (line.length() >= 4 && line.charAt(3) == '-');

  if (code != expectedCode) {
    Serial.printf("SMTP unexpected response (expected %d): %s\n", expectedCode, line.c_str());
    return false;
  }

  return true;
}

bool sendSmtpCommand(WiFiClient& client, const String& command, int expectedCode) {
  client.println(command);
  return readSmtpResponse(client, expectedCode);
}

void sendSmtpNotification(const String& title, const String& message) {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  WiFiClient* client = &plainClient;

  if (SMTP_USE_TLS) {
    secureClient.setInsecure();
    client = &secureClient;
  }

  if (!client->connect(SMTP_SERVER, SMTP_PORT)) {
    Serial.println("Failed to connect to SMTP server");
    return;
  }

  if (!readSmtpResponse(*client, 220)) return;
  if (!sendSmtpCommand(*client, "EHLO esp32-monitor", 250)) return;

  if (strlen(SMTP_USERNAME) > 0) {
    if (!sendSmtpCommand(*client, "AUTH LOGIN", 334)) return;
    if (!sendSmtpCommand(*client, base64Encode(SMTP_USERNAME), 334)) return;
    if (!sendSmtpCommand(*client, base64Encode(SMTP_PASSWORD), 235)) return;
  }

  if (!sendSmtpCommand(*client, "MAIL FROM:<" + String(SMTP_FROM_ADDRESS) + ">", 250)) return;

  String recipients = String(SMTP_TO_ADDRESS);
  recipients.replace(" ", "");
  int start = 0;
  while (start < recipients.length()) {
    int commaIndex = recipients.indexOf(',', start);
    if (commaIndex == -1) commaIndex = recipients.length();
    String address = recipients.substring(start, commaIndex);
    if (address.length() > 0) {
      if (!sendSmtpCommand(*client, "RCPT TO:<" + address + ">", 250)) return;
    }
    start = commaIndex + 1;
  }

  if (!sendSmtpCommand(*client, "DATA", 354)) return;

  client->printf("From: <%s>\r\n", SMTP_FROM_ADDRESS);
  client->printf("To: %s\r\n", SMTP_TO_ADDRESS);
  client->printf("Subject: %s\r\n", title.c_str());
  client->println("Content-Type: text/plain; charset=\"UTF-8\"\r\n");
  client->println(message);
  client->println(".");

  if (!readSmtpResponse(*client, 250)) return;
  sendSmtpCommand(*client, "QUIT", 221);
  client->stop();

  Serial.println("SMTP notification sent");
}

void saveServices() {
  File file = LittleFS.open("/services.json", "w");
  if (!file) {
    Serial.println("Failed to open services.json for writing");
    return;
  }

  JsonDocument doc;
  JsonArray array = doc["services"].to<JsonArray>();

  for (int i = 0; i < serviceCount; i++) {
    JsonObject obj = array.add<JsonObject>();
    obj["id"] = services[i].id;
    obj["name"] = services[i].name;
    obj["type"] = (int)services[i].type;
    obj["host"] = services[i].host;
    obj["port"] = services[i].port;
    obj["path"] = services[i].path;
    obj["expectedResponse"] = services[i].expectedResponse;
    obj["checkInterval"] = services[i].checkInterval;
    obj["passThreshold"] = services[i].passThreshold;
    obj["failThreshold"] = services[i].failThreshold;
  }

  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to serialize services.json");
  }
  file.close();
  Serial.println("Services saved");
}

void loadServices() {
  File file = LittleFS.open("/services.json", "r");
  if (!file) {
    Serial.println("No services.json found, starting fresh");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("Failed to parse services.json");
    return;
  }

  JsonArray array = doc["services"];
  serviceCount = 0;

  for (JsonObject obj : array) {
    if (serviceCount >= MAX_SERVICES) break;

    services[serviceCount].id = obj["id"].as<String>();
    services[serviceCount].name = obj["name"].as<String>();
    services[serviceCount].type = (ServiceType)obj["type"].as<int>();
    services[serviceCount].host = obj["host"].as<String>();
    services[serviceCount].port = obj["port"];
    services[serviceCount].path = obj["path"].as<String>();
    services[serviceCount].expectedResponse = obj["expectedResponse"].as<String>();
    services[serviceCount].checkInterval = obj["checkInterval"];
    services[serviceCount].passThreshold = obj["passThreshold"] | 1;
    services[serviceCount].failThreshold = obj["failThreshold"] | 1;
    services[serviceCount].consecutivePasses = 0;
    services[serviceCount].consecutiveFails = 0;
    services[serviceCount].isUp = false;
    services[serviceCount].lastCheck = 0;
    services[serviceCount].lastUptime = 0;
    services[serviceCount].lastError = "";
    services[serviceCount].secondsSinceLastCheck = -1;

    serviceCount++;
  }

  Serial.printf("Loaded %d services\n", serviceCount);
}

String getServiceTypeString(ServiceType type) {
  switch (type) {
    case TYPE_HOME_ASSISTANT: return "home_assistant";
    case TYPE_JELLYFIN: return "jellyfin";
    case TYPE_HTTP_GET: return "http_get";
    case TYPE_PING: return "ping";
    default: return "unknown";
  }
}

String getWebPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Uptime Monitor</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }

        .container {
            max-width: 1200px;
            margin: 0 auto;
        }

        .header {
            text-align: center;
            color: white;
            margin-bottom: 30px;
        }

        .header h1 {
            font-size: 2.5em;
            margin-bottom: 10px;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.2);
        }

        .header p {
            font-size: 1.1em;
            opacity: 0.9;
        }

        .card {
            background: white;
            border-radius: 12px;
            padding: 25px;
            margin-bottom: 20px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }

        .add-service-form {
            display: grid;
            gap: 15px;
        }

        .form-group {
            display: flex;
            flex-direction: column;
        }

        .form-row {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
        }

        label {
            font-weight: 600;
            margin-bottom: 5px;
            color: #333;
            font-size: 0.9em;
        }

        input, select {
            padding: 10px;
            border: 2px solid #e0e0e0;
            border-radius: 6px;
            font-size: 1em;
            transition: border-color 0.3s;
        }

        input:focus, select:focus {
            outline: none;
            border-color: #667eea;
        }

        .btn {
            padding: 12px 24px;
            border: none;
            border-radius: 6px;
            font-size: 1em;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
        }

        .btn-primary {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
        }

        .btn-primary:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
        }

        .btn-danger {
            background: #ef4444;
            color: white;
            padding: 8px 16px;
            font-size: 0.9em;
        }

        .btn-danger:hover {
            background: #dc2626;
        }

        .btn-secondary {
            background: #6b7280;
            color: white;
        }

        .btn-secondary:hover {
            background: #4b5563;
        }

        .card-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 12px;
            margin-bottom: 20px;
        }

        .backup-actions {
            display: flex;
            gap: 10px;
            align-items: center;
            flex-wrap: wrap;
        }

        .backup-actions input[type="file"] {
            display: none;
        }

        .backup-actions .btn {
            display: inline-flex;
            align-items: center;
            justify-content: center;
            height: 44px;
            padding: 0 18px;
            line-height: 1;
            box-sizing: border-box;
        }

        .services-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
            gap: 20px;
        }

        .service-card {
            background: white;
            border-radius: 12px;
            padding: 20px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
            border-left: 4px solid #e0e0e0;
            transition: all 0.3s;
        }

        .service-card.up {
            border-left-color: #10b981;
        }

        .service-card.down {
            border-left-color: #ef4444;
        }

        .service-card:hover {
            transform: translateY(-4px);
            box-shadow: 0 4px 12px rgba(0,0,0,0.15);
        }

        .service-header {
            display: flex;
            justify-content: space-between;
            align-items: start;
            margin-bottom: 15px;
        }

        .service-name {
            font-size: 1.2em;
            font-weight: 700;
            color: #1f2937;
        }

        .service-status {
            display: inline-block;
            padding: 4px 12px;
            border-radius: 20px;
            font-size: 0.85em;
            font-weight: 600;
        }

        .service-status.up {
            background: #d1fae5;
            color: #065f46;
        }

        .service-status.down {
            background: #fee2e2;
            color: #991b1b;
        }

        .service-info {
            margin-bottom: 10px;
            color: #6b7280;
            font-size: 0.9em;
        }

        .service-info strong {
            color: #374151;
        }

        .service-actions {
            margin-top: 15px;
            padding-top: 15px;
            border-top: 1px solid #e5e7eb;
        }

        .type-badge {
            display: inline-block;
            padding: 4px 10px;
            background: #e0e7ff;
            color: #3730a3;
            border-radius: 6px;
            font-size: 0.8em;
            font-weight: 600;
            margin-bottom: 10px;
        }

        .empty-state {
            text-align: center;
            padding: 60px 20px;
            color: white;
        }

        .empty-state h3 {
            font-size: 1.5em;
            margin-bottom: 10px;
        }

        .hidden {
            display: none;
        }

        .alert {
            padding: 12px 20px;
            border-radius: 6px;
            margin-bottom: 20px;
        }

        .alert-success {
            background: #d1fae5;
            color: #065f46;
        }

        .alert-error {
            background: #fee2e2;
            color: #991b1b;
        }

        @media (max-width: 768px) {
            .form-row {
                grid-template-columns: 1fr;
            }

            .services-grid {
                grid-template-columns: 1fr;
            }

            .header h1 {
                font-size: 1.8em;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>ESP32 Uptime Monitor</h1>
            <p>Monitor your services and infrastructure health</p>
        </div>

        <div id="alertContainer"></div>

        <div class="card">
            <div class="card-header">
                <h2 style="margin: 0; color: #1f2937;">Add New Service</h2>
                <div class="backup-actions">
                    <button type="button" class="btn btn-secondary" onclick="exportServices()">Export Monitors</button>
                    <label class="btn btn-secondary" for="importFile">Import Monitors</label>
                    <input type="file" id="importFile" accept=".json" onchange="importServices(this.files[0])">
                </div>
            </div>
            <form id="addServiceForm" class="add-service-form">
                <div class="form-group">
                    <label for="serviceName">Service Name</label>
                    <input type="text" id="serviceName" required placeholder="My Service">
                </div>

                <div class="form-row">
                    <div class="form-group">
                        <label for="serviceType">Service Type</label>
                        <select id="serviceType" required>
                            <option value="home_assistant">Home Assistant</option>
                            <option value="jellyfin">Jellyfin</option>
                            <option value="http_get">HTTP GET</option>
                            <option value="ping">Ping</option>
                        </select>
                    </div>

                    <div class="form-group">
                        <label for="serviceHost">Host / IP Address</label>
                        <input type="text" id="serviceHost" required placeholder="192.168.1.100">
                    </div>
                </div>

                <div class="form-row">
                    <div class="form-group">
                        <label for="servicePort">Port</label>
                        <input type="number" id="servicePort" value="80" required>
                    </div>

                    <div class="form-group">
                        <label for="checkInterval">Check Interval (seconds)</label>
                        <input type="number" id="checkInterval" value="60" required min="10">
                    </div>
                </div>

                <div class="form-row">
                    <div class="form-group">
                        <label for="failThreshold">Fail Threshold</label>
                        <input type="number" id="failThreshold" value="1" required min="1" title="Number of consecutive failures before marking as DOWN">
                    </div>

                    <div class="form-group">
                        <label for="passThreshold">Pass Threshold</label>
                        <input type="number" id="passThreshold" value="1" required min="1" title="Number of consecutive successes before marking as UP">
                    </div>
                </div>

                <div class="form-group" id="pathGroup">
                    <label for="servicePath">Path</label>
                    <input type="text" id="servicePath" value="/" placeholder="/">
                </div>

                <div class="form-group" id="responseGroup">
                    <label for="expectedResponse">Expected Response (* for any)</label>
                    <input type="text" id="expectedResponse" value="*" placeholder="*">
                </div>

                <button type="submit" class="btn btn-primary">Add Service</button>
            </form>
        </div>

        <h2 style="color: white; margin-bottom: 20px; font-size: 1.5em;">Monitored Services</h2>
        <div id="servicesContainer" class="services-grid"></div>
        <div id="emptyState" class="empty-state hidden">
            <h3>No services yet</h3>
            <p>Add your first service using the form above</p>
        </div>
    </div>

    <script>
        let services = [];

        // Update form fields based on service type
        document.getElementById('serviceType').addEventListener('change', function() {
            const type = this.value;
            const pathGroup = document.getElementById('pathGroup');
            const responseGroup = document.getElementById('responseGroup');
            const portInput = document.getElementById('servicePort');

            if (type === 'ping') {
                pathGroup.classList.add('hidden');
                responseGroup.classList.add('hidden');
            } else {
                pathGroup.classList.remove('hidden');

                if (type === 'http_get') {
                    responseGroup.classList.remove('hidden');
                } else {
                    responseGroup.classList.add('hidden');
                }

                // Set default ports
                // Big benefit of the defined types is we can set defaults like these
                if (type === 'home_assistant') {
                    portInput.value = 8123;
                } else if (type === 'jellyfin') {
                    portInput.value = 8096;
                } else {
                    portInput.value = 80;
                }
            }
        });

        // Add service
        document.getElementById('addServiceForm').addEventListener('submit', async function(e) {
            e.preventDefault();

            const data = {
                name: document.getElementById('serviceName').value,
                type: document.getElementById('serviceType').value,
                host: document.getElementById('serviceHost').value,
                port: parseInt(document.getElementById('servicePort').value),
                path: document.getElementById('servicePath').value,
                expectedResponse: document.getElementById('expectedResponse').value,
                checkInterval: parseInt(document.getElementById('checkInterval').value),
                passThreshold: parseInt(document.getElementById('passThreshold').value),
                failThreshold: parseInt(document.getElementById('failThreshold').value)
            };

            try {
                const response = await fetch('/api/services', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify(data)
                });

                if (response.ok) {
                    showAlert('Service added successfully!', 'success');
                    this.reset();
                    document.getElementById('serviceType').dispatchEvent(new Event('change'));
                    loadServices();
                } else {
                    showAlert('Failed to add service', 'error');
                }
            } catch (error) {
                showAlert('Error: ' + error.message, 'error');
            }
        });

        // Load services
        async function loadServices() {
            try {
                const response = await fetch('/api/services');
                const data = await response.json();
                services = data.services || [];
                renderServices();
            } catch (error) {
                console.error('Error loading services:', error);
            }
        }

        // Render services
        function renderServices() {
            const container = document.getElementById('servicesContainer');
            const emptyState = document.getElementById('emptyState');

            if (services.length === 0) {
                container.innerHTML = '';
                emptyState.classList.remove('hidden');
                return;
            }

            emptyState.classList.add('hidden');

            container.innerHTML = services.map(service => {
                let uptimeStr = 'Not checked yet';

                if (service.secondsSinceLastCheck >= 0) {
                    const seconds = service.secondsSinceLastCheck;
                    if (seconds < 60) {
                        uptimeStr = `${seconds}s ago`;
                    } else if (seconds < 3600) {
                        const minutes = Math.floor(seconds / 60);
                        const secs = seconds % 60;
                        uptimeStr = `${minutes}m ${secs}s ago`;
                    } else {
                        const hours = Math.floor(seconds / 3600);
                        const minutes = Math.floor((seconds % 3600) / 60);
                        uptimeStr = `${hours}h ${minutes}m ago`;
                    }
                }

                return `
                    <div class="service-card ${service.isUp ? 'up' : 'down'}">
                        <div class="service-header">
                            <div>
                                <div class="service-name">${service.name}</div>
                                <div class="type-badge">${service.type.replace('_', ' ').toUpperCase()}</div>
                            </div>
                            <span class="service-status ${service.isUp ? 'up' : 'down'}">
                                ${service.isUp ? 'UP' : 'DOWN'}
                            </span>
                        </div>
                        <div class="service-info">
                            <strong>Host:</strong> ${service.host}:${service.port}
                        </div>
                        ${service.path && service.type !== 'ping' ? `
                        <div class="service-info">
                            <strong>Path:</strong> ${service.path}
                        </div>
                        ` : ''}
                        <div class="service-info">
                            <strong>Check Interval:</strong> ${service.checkInterval}s
                        </div>
                        <div class="service-info">
                            <strong>Thresholds:</strong> ${service.failThreshold} fail / ${service.passThreshold} pass
                        </div>
                        <div class="service-info">
                            <strong>Consecutive:</strong> ${service.consecutivePasses} passes / ${service.consecutiveFails} fails
                        </div>
                        <div class="service-info">
                            <strong>Last Check:</strong> ${uptimeStr}
                        </div>
                        ${service.lastError ? `
                        <div class="service-info" style="color: #ef4444;">
                            <strong>Error:</strong> ${service.lastError}
                        </div>
                        ` : ''}
                        <div class="service-actions">
                            <button class="btn btn-danger" onclick="deleteService('${service.id}')">Delete</button>
                        </div>
                    </div>
                `;
            }).join('');
        }

        // Delete service
        async function deleteService(id) {
            if (!confirm('Are you sure you want to delete this service?')) {
                return;
            }

            try {
                const response = await fetch(`/api/services/${id}`, {
                    method: 'DELETE'
                });

                if (response.ok) {
                    showAlert('Service deleted successfully', 'success');
                    loadServices();
                } else {
                    showAlert('Failed to delete service', 'error');
                }
            } catch (error) {
                showAlert('Error: ' + error.message, 'error');
            }
        }

        // Show alert
        function showAlert(message, type) {
            const container = document.getElementById('alertContainer');
            const alert = document.createElement('div');
            alert.className = `alert alert-${type}`;
            alert.textContent = message;
            container.appendChild(alert);

            setTimeout(() => {
                alert.remove();
            }, 3000);
        }

        // Export services
        function exportServices() {
            window.location.href = '/api/export';
        }

        // Import services
        async function importServices(file) {
            if (!file) return;

            try {
                const text = await file.text();
                const response = await fetch('/api/import', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: text
                });

                const result = await response.json();

                if (response.ok) {
                    showAlert(`Imported ${result.imported} service(s)` + 
                        (result.skipped > 0 ? `, skipped ${result.skipped}` : ''), 'success');
                    loadServices();
                } else {
                    showAlert('Import failed: ' + (result.error || 'Unknown error'), 'error');
                }
            } catch (error) {
                showAlert('Error: ' + error.message, 'error');
            }

            // Reset file input
            document.getElementById('importFile').value = '';
        }

        // Auto-refresh services every 5 seconds
        setInterval(loadServices, 5000);

        // Initial load
        loadServices();
        document.getElementById('serviceType').dispatchEvent(new Event('change'));
    </script>
</body>
</html>
)rawliteral";
}
