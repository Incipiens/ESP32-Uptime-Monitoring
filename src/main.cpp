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

void sendNtfyNotification(const String& title, const String& message, const String& tags = "warning,monitor");
void sendDiscordNotification(const String& title, const String& message);
void sendSmtpNotification(const String& title, const String& message);
void sendMeshCoreNotification(const String& title, const String& message);
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
BLERemoteCharacteristic* meshMessageCharacteristic = nullptr;
bool meshDeviceConnected = false;
unsigned long lastMeshConnectAttempt = 0;
String lastMeshError = "";
bool meshChannelReady = false;
const int BLE_MTU_NEGOTIATION_DELAY_MS = 1500;
const int BLE_SERVICE_DISCOVERY_RETRIES = 3;
const int BLE_SERVICE_DISCOVERY_RETRY_DELAY_MS = 500;
const int BLE_DEINIT_CLEANUP_DELAY_MS = 100; // Allow BLE stack to complete cleanup before deinit

// BLE/WiFi coexistence - ESP32-S3 cannot run WiFi and BLE simultaneously
bool bleOperationInProgress = false;
bool monitoringPaused = false;
bool bleInitialized = false;

// UUIDs derived from random generator to avoid collisions
const char* MESHCORE_SERVICE_UUID = "8b9b0b3d-1e1d-4e91-9c23-4c1e1a4f0a2d";
const char* MESHCORE_MESSAGE_CHAR_UUID = "0b5ad4e1-a62f-41a8-99a1-86a9b8b43964";

class MeshClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* client) override {
    meshDeviceConnected = true;
    lastMeshError = "";
  }

  void onDisconnect(BLEClient* client) override {
    meshDeviceConnected = false;
    meshChannelReady = false;
  }
};

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

  // Note: BLE is NOT initialized at startup to avoid conflicts with WiFi.
  // ESP32-S3 cannot run WiFi and BLE simultaneously. BLE will be
  // initialized on-demand when a MeshCore notification is needed.

  // Initialize WiFi
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
  BLEDevice::init(BLE_DEVICE_NAME);
  
  // Verify initialization succeeded before continuing
  if (!BLEDevice::getInitialized()) {
    Serial.println("BLE initialization failed");
    return;
  }
  bleInitialized = true;

  // Set up PIN-based pairing to satisfy MeshCore's security expectations
  BLESecurity* security = new BLESecurity();
  security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security->setCapability(ESP_IO_CAP_OUT);
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setStaticPIN(BLE_PAIRING_PIN);

  connectToMeshCore();
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
  meshMessageCharacteristic = nullptr;
  meshChannelReady = false;

  Serial.printf("Scanning for MeshCore peer named '%s' (extended debug)...\n", BLE_PEER_NAME);

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);

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

    // Matching logic: exact name, substring, or advertised service UUID match
    bool nameMatches = (devName.length() && devName == String(BLE_PEER_NAME));
    bool nameContains = (devName.length() && devName.indexOf(String(BLE_PEER_NAME)) >= 0);
    bool serviceMatches = false;

    if (device.haveServiceUUID()) {
      BLEUUID adv = device.getServiceUUID();
      // Compare to configured MeshCore UUID (safe regardless of how MESHCORE_SERVICE_UUID is defined)
      serviceMatches = adv.equals(BLEUUID(MESHCORE_SERVICE_UUID));
    }

    if (!(nameMatches || nameContains || serviceMatches)) {
      Serial.println("       Not the MeshCore peer (name/service mismatch), skipping.");
      continue;
    }

    Serial.println("MeshCore peer candidate found, attempting connection...");

    if (meshClient == nullptr) {
      meshClient = BLEDevice::createClient();
      meshClient->setClientCallbacks(new MeshClientCallbacks());
    }

    if (!meshClient->connect(&device)) {
      lastMeshError = "Connection attempt failed";
      Serial.println("Connection attempt failed");
      continue;
    }

    // Allow MTU negotiation to complete before accessing services
    delay(BLE_MTU_NEGOTIATION_DELAY_MS);

    // Retry service discovery to handle cases where MTU negotiation may still be completing
    BLERemoteService* service = nullptr;
    for (int retry = 0; retry < BLE_SERVICE_DISCOVERY_RETRIES; retry++) {
      service = meshClient->getService(MESHCORE_SERVICE_UUID);
      if (service != nullptr) {
        break;
      }
      if (retry < BLE_SERVICE_DISCOVERY_RETRIES - 1) {
        Serial.printf("Service discovery attempt %d failed, retrying...\n", retry + 1);
        delay(BLE_SERVICE_DISCOVERY_RETRY_DELAY_MS);
      }
    }
    if (service == nullptr) {
      lastMeshError = "MeshCore service not found on peer";
      Serial.println("MeshCore service not found on peer, disconnecting...");
      meshClient->disconnect();
      continue;
    }

    BLERemoteCharacteristic* characteristic = service->getCharacteristic(MESHCORE_MESSAGE_CHAR_UUID);
    if (characteristic == nullptr) {
      lastMeshError = "MeshCore message characteristic missing";
      Serial.println("MeshCore message characteristic missing, disconnecting...");
      meshClient->disconnect();
      continue;
    }

    if (!characteristic->canWrite()) {
      lastMeshError = "MeshCore message characteristic is not writable";
      Serial.println("MeshCore message characteristic is not writable, disconnecting...");
      meshClient->disconnect();
      continue;
    }

    meshMessageCharacteristic = characteristic;
    meshDeviceConnected = true;
    lastMeshError = "";
    Serial.println("Connected to MeshCore peer and ready to send messages");

    if (!ensureMeshChannel()) {
      Serial.println("ensureMeshChannel() failed, disconnecting...");
      meshClient->disconnect();
      meshDeviceConnected = false;
      return false;
    }

    return true;
  }

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
  meshMessageCharacteristic = nullptr;
  meshChannelReady = false;
  
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
  if (meshChannelReady) {
    return true;
  }

  if (!meshDeviceConnected || meshMessageCharacteristic == nullptr) {
    return false;
  }

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

  JsonDocument doc;
  doc["action"] = "ensure_channel";
  doc["channel"] = BLE_MESH_CHANNEL_NAME;
  doc["key"] = BLE_MESH_CHANNEL_KEY;

  String payload;
  serializeJson(doc, payload);

  Serial.printf("Provisioning MeshCore channel '%s'...\n", BLE_MESH_CHANNEL_NAME);
  try {
    meshMessageCharacteristic->writeValue(
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(payload.c_str())),
      payload.length(),
      false
    );
  } catch (...) {
    lastMeshError = "Failed to provision MeshCore channel";
    return false;
  }

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
  if (!isNtfyConfigured() && !isDiscordConfigured() && !isSmtpConfigured() && !meshDeviceConnected) {
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
  if (!isNtfyConfigured() && !isDiscordConfigured() && !isSmtpConfigured() && !meshDeviceConnected) {
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
  if (meshDeviceConnected && meshMessageCharacteristic != nullptr) {
    if (ensureMeshChannel()) {
      JsonDocument doc;
      doc["channel"] = BLE_MESH_CHANNEL_NAME;
      doc["key"] = BLE_MESH_CHANNEL_KEY;
      doc["title"] = title;
      doc["body"] = message;

      String payload;
      serializeJson(doc, payload);

      try {
        meshMessageCharacteristic->writeValue(
          const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(payload.c_str())),
          payload.length(),
          false
        );
        Serial.println("MeshCore notification sent successfully");
      } catch (...) {
        Serial.println("MeshCore notification failed: write error");
      }
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
