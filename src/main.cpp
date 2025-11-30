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
#include <WiFiUdp.h>
#include <Arduino_SNMP_Manager.h>
#include <regex.h>

// MeshCore layered protocol implementation
#include "MeshCore.hpp"

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
void initMeshCore();
void disconnectMeshCore();
void disconnectWiFi();
void reconnectWiFi();

AsyncWebServer server(80);

// MeshCore Protocol Stack (layered architecture)
// Layer 1: BLE Transport - handles BLE connection and I/O
// Layer 2: Frame Codec - handles frame parsing/building  
// Layer 3: Companion Protocol - handles MeshCore protocol logic
static BLECentralTransport* meshTransport = nullptr;
static FrameCodec* meshCodec = nullptr;
static CompanionProtocol* meshProtocol = nullptr;

// BLE/WiFi coexistence - ESP32-S3 cannot run WiFi and BLE simultaneously
bool bleOperationInProgress = false;
bool monitoringPaused = false;

// Pending MeshCore notification - used to defer BLE operations from HTTP handlers
// This prevents task watchdog timeouts by allowing the async web server to complete
// HTTP response delivery before WiFi is disconnected for BLE operations.
volatile bool pendingMeshNotification = false;
String pendingMeshTitle = "";
String pendingMeshMessage = "";

// Helper functions to access protocol state (for API compatibility)
bool isMeshDeviceConnected() {
  return meshTransport != nullptr && meshTransport->isConnected();
}

bool isMeshChannelReady() {
  return meshProtocol != nullptr && meshProtocol->isChannelReady();
}

String getMeshLastError() {
  if (meshProtocol != nullptr) {
    return meshProtocol->getLastError();
  }
  if (meshTransport != nullptr) {
    return meshTransport->getLastError();
  }
  return "";
}

int getMeshProtocolState() {
  if (meshProtocol != nullptr) {
    return static_cast<int>(meshProtocol->getState());
  }
  return 0;
}

// Service types
// Right now the behavior for each is rudimentary
// However, you can use this to expand and add services with more complex checks
enum ServiceType {
  TYPE_HTTP_GET,
  TYPE_PING,
  TYPE_SNMP_GET
};

// SNMP comparison operators for value checks
enum SnmpCompareOp {
  SNMP_OP_EQ,    // Equal (=)
  SNMP_OP_NE,    // Not equal (<>)
  SNMP_OP_LT,    // Less than (<)
  SNMP_OP_LE,    // Less than or equal (<=)
  SNMP_OP_GT,    // Greater than (>)
  SNMP_OP_GE     // Greater than or equal (>=)
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
  int rearmCount;         // Number of failed checks before re-alerting (0 = disabled)
  int consecutivePasses;  // Current count of consecutive passes
  int consecutiveFails;   // Current count of consecutive fails
  int failedChecksSinceAlert; // Failed checks since last alert (for re-arm)
  bool isUp;
  unsigned long lastCheck;
  unsigned long lastUptime;
  String lastError;
  int secondsSinceLastCheck;
  // SNMP-specific fields
  String snmpOid;         // SNMP OID to query (e.g., "1.3.6.1.2.1.1.1.0")
  String snmpCommunity;   // SNMP community string (default: "public")
  SnmpCompareOp snmpCompareOp;  // Comparison operator for SNMP value check
  String snmpExpectedValue;     // Expected value for comparison
};

// Store up to 20 services
const int MAX_SERVICES = 20;
Service services[MAX_SERVICES];
int serviceCount = 0;

// Regex matching constants
const int MAX_REGEX_PATTERN_LENGTH = 256;
const char* REGEX_PREFIX = "regex:";
const int REGEX_PREFIX_LENGTH = 6;

// Notification queue for failed notifications
// Only stores the latest notification per service (isUp state)
// Tracks which notification channels have failed
struct QueuedNotification {
  String serviceId;      // Service ID this notification is for
  String title;          // Notification title
  String message;        // Notification message
  bool isUp;             // true = online notification, false = offline notification
  String tags;           // ntfy tags (for ntfy notifications)
  bool ntfyPending;      // Needs to be sent via ntfy
  bool discordPending;   // Needs to be sent via Discord
  bool smtpPending;      // Needs to be sent via SMTP
  bool meshPending;      // Needs to be sent via MeshCore
  unsigned long lastRetry; // Last retry attempt timestamp
};

const int MAX_QUEUED_NOTIFICATIONS = MAX_SERVICES;
QueuedNotification notificationQueue[MAX_QUEUED_NOTIFICATIONS];
int queuedNotificationCount = 0;

// Retry interval for failed notifications (30 seconds for WiFi-based)
const unsigned long NOTIFICATION_RETRY_INTERVAL = 30000;

// MeshCore retry interval (10 minutes to prevent frequent WiFi disconnects)
const unsigned long MESHCORE_RETRY_INTERVAL = 600000;
unsigned long lastMeshCoreRetry = 0;

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
bool checkHttpGet(Service& service);
bool checkPing(Service& service);
bool checkSnmpGet(Service& service);
int matchesRegex(const String& text, const String& pattern);
String getWebPage();
String getServiceTypeString(ServiceType type);
String getSnmpCompareOpString(SnmpCompareOp op);
SnmpCompareOp parseSnmpCompareOp(const String& opStr);
String base64Encode(const String& input);
bool readSmtpResponse(WiFiClient& client, int expectedCode);
bool sendSmtpCommand(WiFiClient& client, const String& command, int expectedCode);
void sendBootNotification();

// Notification sending functions that return success status
bool sendNtfyNotificationWithStatus(const String& title, const String& message, const String& tags);
bool sendDiscordNotificationWithStatus(const String& title, const String& message);
bool sendSmtpNotificationWithStatus(const String& title, const String& message);
bool sendMeshCoreNotificationWithStatus(const String& title, const String& message);

// Notification queue functions
void queueNotification(const String& serviceId, const String& title, const String& message, 
                       bool isUp, const String& tags, bool ntfyFailed, bool discordFailed, 
                       bool smtpFailed, bool meshFailed);
void processNotificationQueue();
void processMeshCoreQueue();
int findQueuedNotification(const String& serviceId);
void removeQueuedNotification(int index);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting ESP32 Uptime Monitor...");

  // Initialize filesystem
  initFileSystem();

  // Initialize WiFi
  initWiFi();

  // Load saved services
  loadServices();

  // Initialize web server
  initWebServer();

  // Send boot notification if enabled and WiFi is connected
  if (BOOT_NOTIFICATION_ENABLED) {
    if (WiFi.status() == WL_CONNECTED) {
      sendBootNotification();
    } else {
      Serial.println("Boot notification skipped: WiFi not connected");
    }
  }

  Serial.println("System ready!");
  Serial.print("Access web interface at: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  static unsigned long lastCheckTime = 0;
  unsigned long currentTime = millis();

  // Process pending MeshCore notifications from HTTP handlers
  // This runs in the main loop to avoid blocking the async web server
  // and prevents task watchdog timeouts from WiFi/BLE switching during HTTP responses
  if (pendingMeshNotification && !bleOperationInProgress) {
    // Copy values and clear flag atomically to prevent race conditions with HTTP handler.
    // The HTTP handler checks both pendingMeshNotification and bleOperationInProgress
    // before writing, so setting the flag false first prevents new writes.
    // We use noInterrupts()/interrupts() to ensure the copy is atomic since
    // the async web server runs in a separate FreeRTOS task.
    noInterrupts();
    pendingMeshNotification = false;
    String title = pendingMeshTitle;
    String message = pendingMeshMessage;
    pendingMeshTitle = "";
    pendingMeshMessage = "";
    interrupts();
    
    sendMeshCoreNotification(title, message);
  }

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

  // Process notification queue for failed notifications (WiFi-based)
  processNotificationQueue();
  
  // Process MeshCore queue separately (batched, 10 minute interval)
  processMeshCoreQueue();

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

void initMeshCore() {
  Serial.println("Initializing MeshCore protocol stack...");
  Serial.printf("Free heap before init: %d bytes\n", ESP.getFreeHeap());
  
  // Create the layered protocol stack
  BLECentralTransport::Config config;
  config.deviceName = BLE_DEVICE_NAME;
  config.peerName = BLE_PEER_NAME;
  config.pairingPin = BLE_PAIRING_PIN;
  
  meshTransport = new BLECentralTransport(config);
  meshCodec = new FrameCodec(*meshTransport);
  meshProtocol = new CompanionProtocol(*meshTransport, *meshCodec);
  
  // Initialize and connect
  if (!meshTransport->init()) {
    Serial.println("ERROR: BLE initialization failed");
    disconnectMeshCore();  // Clean up allocated objects
    return;
  }
  
  if (!meshTransport->connect()) {
    Serial.println("MeshCore connection failed");
    disconnectMeshCore();  // Clean up allocated objects
    return;
  }
  
  // Start session
  if (!meshProtocol->startSession("ESP32-Uptime")) {
    Serial.println("MeshCore session start failed");
    disconnectMeshCore();  // Clean up allocated objects
    return;
  }
  
  // Find the configured channel
  uint8_t channelIdx;
  if (!meshProtocol->findChannelByName(BLE_MESH_CHANNEL_NAME, channelIdx)) {
    Serial.println("Channel not found");
    disconnectMeshCore();  // Clean up allocated objects
    return;
  }
  
  Serial.printf("MeshCore ready on channel %d\n", channelIdx);
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

  // Second attempt: begin(true) formats automatically on mount failure and
  // handles partition initialization internally, which is more reliable than
  // calling format() directly on corrupted or uninitialized flash.
  if (LittleFS.begin(true)) {
    Serial.println("LittleFS formatted and mounted successfully");
    return;
  }

  Serial.println("LittleFS format via begin(true) failed, trying explicit format...");

  // Third attempt: explicit format for edge cases where begin(true) fails
  // This can happen if the partition table is misconfigured
  if (!LittleFS.format()) {
    Serial.println("LittleFS format failed! Check partition table and flash configuration.");
    Serial.println("Ensure a 'littlefs' data partition exists in partitions.csv and matches your flash size.");
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

void disconnectMeshCore() {
  // Clean up the layered protocol stack
  if (meshProtocol != nullptr) {
    delete meshProtocol;
    meshProtocol = nullptr;
  }
  
  if (meshCodec != nullptr) {
    delete meshCodec;
    meshCodec = nullptr;
  }
  
  if (meshTransport != nullptr) {
    meshTransport->disconnect();
    meshTransport->deinit();
    delete meshTransport;
    meshTransport = nullptr;
  }
  
  Serial.println("MeshCore disconnected and deinitialized");
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

void initWebServer() {

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!ensureAuthenticated(request)) {
      return;
    }
    request->send(200, "text/html", getWebPage());
  });

  server.on("/api/mesh/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["connected"] = isMeshDeviceConnected();
    doc["peerName"] = BLE_PEER_NAME;
    doc["deviceName"] = BLE_DEVICE_NAME;
    doc["channel"] = BLE_MESH_CHANNEL_NAME;
    doc["channelReady"] = isMeshChannelReady();
    doc["protocolState"] = getMeshProtocolState();
    doc["lastError"] = getMeshLastError();
    doc["bleOperationInProgress"] = bleOperationInProgress;
    doc["pendingNotification"] = (bool)pendingMeshNotification;
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

      // Check if a BLE operation is already in progress or queued
      if (bleOperationInProgress || pendingMeshNotification) {
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

      // Queue the notification to be processed in the main loop.
      // This prevents task watchdog timeouts by allowing the async web server
      // to fully complete HTTP response delivery before WiFi is disconnected
      // for BLE operations. The ESP32-S3 cannot run WiFi and BLE simultaneously,
      // so we must ensure the HTTP response is sent before switching to BLE.
      // Use noInterrupts()/interrupts() to ensure atomic write since the main
      // loop() reads these variables from a different FreeRTOS task context.
      noInterrupts();
      pendingMeshTitle = title;
      pendingMeshMessage = message;
      pendingMeshNotification = true;
      interrupts();
      
      request->send(202, "application/json", "{\"success\":true,\"status\":\"queued\"}");
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
      obj["rearmCount"] = services[i].rearmCount;
      obj["consecutivePasses"] = services[i].consecutivePasses;
      obj["consecutiveFails"] = services[i].consecutiveFails;
      obj["failedChecksSinceAlert"] = services[i].failedChecksSinceAlert;
      obj["isUp"] = services[i].isUp;
      obj["secondsSinceLastCheck"] = services[i].secondsSinceLastCheck;
      obj["lastError"] = services[i].lastError;
      // SNMP-specific fields
      obj["snmpOid"] = services[i].snmpOid;
      obj["snmpCommunity"] = services[i].snmpCommunity;
      obj["snmpCompareOp"] = getSnmpCompareOpString(services[i].snmpCompareOp);
      obj["snmpExpectedValue"] = services[i].snmpExpectedValue;
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
      if (typeStr == "http_get") {
        newService.type = TYPE_HTTP_GET;
      } else if (typeStr == "ping") {
        newService.type = TYPE_PING;
      } else if (typeStr == "snmp_get") {
        newService.type = TYPE_SNMP_GET;
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

      int rearmCount = doc["rearmCount"] | 0;
      if (rearmCount < 0) rearmCount = 0;
      newService.rearmCount = rearmCount;

      // SNMP-specific fields
      newService.snmpOid = doc["snmpOid"] | "";
      newService.snmpCommunity = doc["snmpCommunity"] | "public";
      String compareOpStr = doc["snmpCompareOp"] | "=";
      newService.snmpCompareOp = parseSnmpCompareOp(compareOpStr);
      newService.snmpExpectedValue = doc["snmpExpectedValue"] | "";

      newService.consecutivePasses = 0;
      newService.consecutiveFails = 0;
      newService.failedChecksSinceAlert = 0;
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
      obj["rearmCount"] = services[i].rearmCount;
      // SNMP-specific fields
      obj["snmpOid"] = services[i].snmpOid;
      obj["snmpCommunity"] = services[i].snmpCommunity;
      obj["snmpCompareOp"] = getSnmpCompareOpString(services[i].snmpCompareOp);
      obj["snmpExpectedValue"] = services[i].snmpExpectedValue;
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
        if (typeStr == "http_get") {
          type = TYPE_HTTP_GET;
        } else if (typeStr == "ping") {
          type = TYPE_PING;
        } else if (typeStr == "snmp_get") {
          type = TYPE_SNMP_GET;
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

        int rearmCount = obj["rearmCount"] | 0;
        if (rearmCount < 0) rearmCount = 0;

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
        newService.rearmCount = rearmCount;
        // SNMP-specific fields
        newService.snmpOid = obj["snmpOid"] | "";
        newService.snmpCommunity = obj["snmpCommunity"] | "public";
        String compareOpStr = obj["snmpCompareOp"] | "=";
        newService.snmpCompareOp = parseSnmpCompareOp(compareOpStr);
        newService.snmpExpectedValue = obj["snmpExpectedValue"] | "";
        newService.consecutivePasses = 0;
        newService.consecutiveFails = 0;
        newService.failedChecksSinceAlert = 0;
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
      case TYPE_HTTP_GET:
        checkResult = checkHttpGet(services[i]);
        break;
      case TYPE_PING:
        checkResult = checkPing(services[i]);
        break;
      case TYPE_SNMP_GET:
        checkResult = checkSnmpGet(services[i]);
        break;
    }

    // Update consecutive counters based on check result
    if (checkResult) {
      services[i].consecutivePasses++;
      services[i].consecutiveFails = 0;
      services[i].lastUptime = currentTime;
      services[i].lastError = "";
      services[i].failedChecksSinceAlert = 0;  // Reset re-arm counter on success
    } else {
      services[i].consecutiveFails++;
      services[i].consecutivePasses = 0;
    }

    // Determine new state based on thresholds
    if (!services[i].isUp && services[i].consecutivePasses >= services[i].passThreshold) {
      // Service has passed enough times to be considered UP
      services[i].isUp = true;
      services[i].failedChecksSinceAlert = 0;  // Reset re-arm counter on recovery
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
        services[i].failedChecksSinceAlert = 0;  // Reset counter after initial alert
      } else if (!firstCheck) {
        sendOnlineNotification(services[i]);
      }
    } else if (!services[i].isUp && !checkResult && services[i].rearmCount > 0) {
      // Service is still DOWN and check failed - handle re-arm logic
      services[i].failedChecksSinceAlert++;
      if (services[i].failedChecksSinceAlert >= services[i].rearmCount) {
        Serial.printf("Service '%s' still DOWN - re-arming alert after %d failed checks\n",
          services[i].name.c_str(), services[i].failedChecksSinceAlert);
        sendOfflineNotification(services[i]);
        services[i].failedChecksSinceAlert = 0;  // Reset counter after re-arm alert
      }
    }
  }
}

// Helper function to match text against a POSIX extended regex pattern
// Returns 0 on match, 1 on no match, -1 on pattern too long, -2 on invalid pattern
int matchesRegex(const String& text, const String& pattern) {
  // Limit pattern length to prevent excessive resource usage
  if (pattern.length() > MAX_REGEX_PATTERN_LENGTH) {
    return -1;
  }
  
  regex_t regex;
  int result;
  
  // Compile the regex pattern with extended syntax
  result = regcomp(&regex, pattern.c_str(), REG_EXTENDED | REG_NOSUB);
  if (result != 0) {
    // Pattern compilation failed - do not call regfree() on failed compile
    return -2;
  }
  
  // Execute the regex match
  result = regexec(&regex, text.c_str(), 0, NULL, 0);
  regfree(&regex);
  
  return result == 0 ? 0 : 1;  // 0 = match, 1 = no match
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
        // Check if expectedResponse is a regex pattern (prefixed with "regex:")
        if (service.expectedResponse.startsWith(REGEX_PREFIX)) {
          String pattern = service.expectedResponse.substring(REGEX_PREFIX_LENGTH);
          int regexResult = matchesRegex(payload, pattern);
          if (regexResult == 0) {
            isUp = true;
          } else if (regexResult == -1) {
            service.lastError = "Regex pattern too long";
          } else if (regexResult == -2) {
            service.lastError = "Invalid regex pattern";
          } else {
            service.lastError = "Regex mismatch";
          }
        } else {
          // Plain substring match
          isUp = payload.indexOf(service.expectedResponse) >= 0;
          if (!isUp) {
            service.lastError = "Response mismatch";
          }
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

// Helper function to compare SNMP values based on operator
bool compareSnmpValue(const String& actualValue, SnmpCompareOp op, const String& expectedValue) {
  // Try numeric comparison first
  bool actualIsNumeric = true;
  bool expectedIsNumeric = true;
  float actualNum = 0;
  float expectedNum = 0;
  
  // Check if actual value is numeric
  if (actualValue.length() > 0) {
    char* endPtr;
    actualNum = strtof(actualValue.c_str(), &endPtr);
    if (*endPtr != '\0') {
      actualIsNumeric = false;
    }
  } else {
    actualIsNumeric = false;
  }
  
  // Check if expected value is numeric
  if (expectedValue.length() > 0) {
    char* endPtr;
    expectedNum = strtof(expectedValue.c_str(), &endPtr);
    if (*endPtr != '\0') {
      expectedIsNumeric = false;
    }
  } else {
    expectedIsNumeric = false;
  }
  
  // Use numeric comparison if both are numeric
  if (actualIsNumeric && expectedIsNumeric) {
    switch (op) {
      case SNMP_OP_EQ: return actualNum == expectedNum;
      case SNMP_OP_NE: return actualNum != expectedNum;
      case SNMP_OP_LT: return actualNum < expectedNum;
      case SNMP_OP_LE: return actualNum <= expectedNum;
      case SNMP_OP_GT: return actualNum > expectedNum;
      case SNMP_OP_GE: return actualNum >= expectedNum;
    }
  }
  
  // Fall back to string comparison for non-numeric values
  int cmp = actualValue.compareTo(expectedValue);
  switch (op) {
    case SNMP_OP_EQ: return cmp == 0;
    case SNMP_OP_NE: return cmp != 0;
    case SNMP_OP_LT: return cmp < 0;
    case SNMP_OP_LE: return cmp <= 0;
    case SNMP_OP_GT: return cmp > 0;
    case SNMP_OP_GE: return cmp >= 0;
  }
  
  return false;
}

bool checkSnmpGet(Service& service) {
  // Resolve hostname to IP
  IPAddress targetIP;
  if (!WiFi.hostByName(service.host.c_str(), targetIP)) {
    service.lastError = "DNS resolution failed";
    return false;
  }
  
  // Create SNMP objects for this request
  WiFiUDP udp;
  SNMPManager snmpManager(service.snmpCommunity.c_str());
  SNMPGet snmpRequest(service.snmpCommunity.c_str(), 1);  // SNMP version 2c (0=v1, 1=v2c)
  
  // Initialize SNMP manager
  snmpManager.setUDP(&udp);
  snmpManager.begin();
  
  // Use local variables to avoid thread safety issues
  // Use special marker values to detect if they were updated
  char stringValue[256];
  char* stringValuePtr = stringValue;
  stringValue[0] = '\0';
  
  // Use special marker value that's unlikely to be a real SNMP response
  // INT32_MIN is used as a sentinel to detect if the value was updated
  int intValue = INT32_MIN;
  
  String responseValue = "";
  bool gotResponse = false;
  
  // Add handler for string values (covers most SNMP types including OctetString)
  ValueCallback* stringCallback = snmpManager.addStringHandler(targetIP, service.snmpOid.c_str(), &stringValuePtr);
  
  // Also add integer handler for numeric values
  ValueCallback* intCallback = snmpManager.addIntegerHandler(targetIP, service.snmpOid.c_str(), &intValue);
  
  // Build and send SNMP request
  snmpRequest.addOIDPointer(stringCallback);
  snmpRequest.setIP(WiFi.localIP());
  snmpRequest.setUDP(&udp);
  snmpRequest.setRequestID(random(1, 65535));
  
  if (!snmpRequest.sendTo(targetIP)) {
    service.lastError = "Failed to send SNMP request";
    return false;
  }
  snmpRequest.clearOIDList();
  
  // Wait for response with timeout (5 seconds)
  unsigned long startTime = millis();
  const unsigned long timeout = 5000;
  
  while (millis() - startTime < timeout) {
    snmpManager.loop();
    
    // Check if we got a string response (non-empty string)
    if (stringValue[0] != '\0') {
      responseValue = String(stringValue);
      gotResponse = true;
      break;
    }
    
    // Check if we got an integer response (value changed from sentinel)
    if (intValue != INT32_MIN) {
      responseValue = String(intValue);
      gotResponse = true;
      break;
    }
    
    delay(10);
  }
  
  if (!gotResponse) {
    service.lastError = "SNMP timeout";
    return false;
  }
  
  // Compare the received value with expected value
  bool success = compareSnmpValue(responseValue, service.snmpCompareOp, service.snmpExpectedValue);
  
  if (!success) {
    service.lastError = "Value mismatch: got '" + responseValue + "', expected " + 
                        getSnmpCompareOpString(service.snmpCompareOp) + " '" + 
                        service.snmpExpectedValue + "'";
  }
  
  return success;
}

String getSnmpCompareOpString(SnmpCompareOp op) {
  switch (op) {
    case SNMP_OP_EQ: return "=";
    case SNMP_OP_NE: return "<>";
    case SNMP_OP_LT: return "<";
    case SNMP_OP_LE: return "<=";
    case SNMP_OP_GT: return ">";
    case SNMP_OP_GE: return ">=";
    default: return "=";
  }
}

SnmpCompareOp parseSnmpCompareOp(const String& opStr) {
  if (opStr == "=" || opStr == "eq") return SNMP_OP_EQ;
  if (opStr == "<>" || opStr == "ne") return SNMP_OP_NE;
  if (opStr == "<" || opStr == "lt") return SNMP_OP_LT;
  if (opStr == "<=" || opStr == "le") return SNMP_OP_LE;
  if (opStr == ">" || opStr == "gt") return SNMP_OP_GT;
  if (opStr == ">=" || opStr == "ge") return SNMP_OP_GE;
  return SNMP_OP_EQ;  // Default to equal
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

  String tags = "warning,monitor";
  bool ntfyFailed = false;
  bool discordFailed = false;
  bool smtpFailed = false;
  bool meshFailed = false;

  if (wifiConnected) {
    if (isNtfyConfigured()) {
      if (!sendNtfyNotificationWithStatus(title, message, tags)) {
        ntfyFailed = true;
      }
    }

    if (isDiscordConfigured()) {
      if (!sendDiscordNotificationWithStatus(title, message)) {
        discordFailed = true;
      }
    }

    if (isSmtpConfigured()) {
      if (!sendSmtpNotificationWithStatus(title, message)) {
        smtpFailed = true;
      }
    }
  } else {
    Serial.println("WiFi offline: queueing internet notifications");
    ntfyFailed = isNtfyConfigured();
    discordFailed = isDiscordConfigured();
    smtpFailed = isSmtpConfigured();
  }

  if (isMeshCoreConfigured()) {
    if (!sendMeshCoreNotificationWithStatus(title, message)) {
      meshFailed = true;
    }
  }

  // Queue any failed notifications for retry
  queueNotification(service.id, title, message, false, tags, 
                    ntfyFailed, discordFailed, smtpFailed, meshFailed);
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

  String tags = "ok,monitor";
  bool ntfyFailed = false;
  bool discordFailed = false;
  bool smtpFailed = false;
  bool meshFailed = false;

  if (wifiConnected) {
    if (isNtfyConfigured()) {
      if (!sendNtfyNotificationWithStatus(title, message, tags)) {
        ntfyFailed = true;
      }
    }

    if (isDiscordConfigured()) {
      if (!sendDiscordNotificationWithStatus(title, message)) {
        discordFailed = true;
      }
    }

    if (isSmtpConfigured()) {
      if (!sendSmtpNotificationWithStatus(title, message)) {
        smtpFailed = true;
      }
    }
  } else {
    Serial.println("WiFi offline: queueing internet notifications");
    ntfyFailed = isNtfyConfigured();
    discordFailed = isDiscordConfigured();
    smtpFailed = isSmtpConfigured();
  }

  if (isMeshCoreConfigured()) {
    if (!sendMeshCoreNotificationWithStatus(title, message)) {
      meshFailed = true;
    }
  }

  // Queue any failed notifications for retry
  queueNotification(service.id, title, message, true, tags, 
                    ntfyFailed, discordFailed, smtpFailed, meshFailed);
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
  
  // Create the layered protocol stack on heap to reduce stack usage.
  // Stack overflow can occur when these large objects are combined with
  // deep BLE callback chains during sendTextMessageToChannel.
  BLECentralTransport::Config config;
  config.deviceName = BLE_DEVICE_NAME;
  config.peerName = BLE_PEER_NAME;
  config.pairingPin = BLE_PAIRING_PIN;
  
  BLECentralTransport* transport = new BLECentralTransport(config);
  FrameCodec* codec = new FrameCodec(*transport);
  CompanionProtocol* protocol = new CompanionProtocol(*transport, *codec);
  
  bool success = false;
  
  // Initialize and connect
  if (transport->init()) {
    if (transport->connect()) {
      // Start session
      if (protocol->startSession("ESP32-Uptime")) {
        // Find the configured channel
        uint8_t channelIdx;
        if (protocol->findChannelByName(BLE_MESH_CHANNEL_NAME, channelIdx)) {
          // Build the message: combine title and message
          String fullMessage = title + ": " + message;
          
          // Send using the protocol layer
          if (protocol->sendTextMessageToChannel(channelIdx, fullMessage)) {
            Serial.println("MeshCore notification sent successfully");
            success = true;
          } else {
            Serial.printf("MeshCore notification failed: send error - %s\n", protocol->getLastError().c_str());
          }
        } else {
          Serial.printf("MeshCore notification skipped: channel not found - %s\n", protocol->getLastError().c_str());
        }
      } else {
        Serial.printf("MeshCore notification skipped: session start failed - %s\n", protocol->getLastError().c_str());
      }
    } else {
      Serial.printf("MeshCore notification skipped: not connected - %s\n", transport->getLastError().c_str());
    }
  } else {
    Serial.printf("MeshCore notification skipped: BLE init failed - %s\n", transport->getLastError().c_str());
  }
  
  // Disconnect BLE and deinitialize to free resources
  transport->disconnect();
  transport->deinit();
  
  // Clean up heap-allocated objects
  delete protocol;
  delete codec;
  delete transport;
  
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

void sendBootNotification() {
  if (!isNtfyConfigured() && !isDiscordConfigured() && !isSmtpConfigured() && !isMeshCoreConfigured()) {
    Serial.println("Boot notification: No notification channels configured");
    return;
  }

  Serial.println("Sending boot notification...");

  String title = "ESP32 Uptime Monitor Started";
  String message = "Device has booted and is now monitoring services.";
  message += " IP: " + WiFi.localIP().toString();

  if (isNtfyConfigured()) {
    sendNtfyNotification(title, message, "rocket,monitor");
  }

  if (isDiscordConfigured()) {
    sendDiscordNotification(title, message);
  }

  if (isSmtpConfigured()) {
    sendSmtpNotification(title, message);
  }

  if (isMeshCoreConfigured()) {
    sendMeshCoreNotification(title, message);
  }

  Serial.println("Boot notification sent");
}

// Notification functions that return success status for queue management

bool sendNtfyNotificationWithStatus(const String& title, const String& message, const String& tags) {
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
  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    Serial.printf("ntfy notification sent: %d\n", httpCode);
    return true;
  } else {
    Serial.printf("Failed to send ntfy notification: %d\n", httpCode);
    return false;
  }
}

bool sendDiscordNotificationWithStatus(const String& title, const String& message) {
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
  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    Serial.printf("Discord notification sent: %d\n", httpCode);
    return true;
  } else {
    Serial.printf("Failed to send Discord notification: %d\n", httpCode);
    return false;
  }
}

bool sendSmtpNotificationWithStatus(const String& title, const String& message) {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  WiFiClient* client = &plainClient;

  if (SMTP_USE_TLS) {
    secureClient.setInsecure();
    client = &secureClient;
  }

  if (!client->connect(SMTP_SERVER, SMTP_PORT)) {
    Serial.println("Failed to connect to SMTP server");
    return false;
  }

  if (!readSmtpResponse(*client, 220)) { client->stop(); return false; }
  if (!sendSmtpCommand(*client, "EHLO esp32-monitor", 250)) { client->stop(); return false; }

  if (strlen(SMTP_USERNAME) > 0) {
    if (!sendSmtpCommand(*client, "AUTH LOGIN", 334)) { client->stop(); return false; }
    if (!sendSmtpCommand(*client, base64Encode(SMTP_USERNAME), 334)) { client->stop(); return false; }
    if (!sendSmtpCommand(*client, base64Encode(SMTP_PASSWORD), 235)) { client->stop(); return false; }
  }

  if (!sendSmtpCommand(*client, "MAIL FROM:<" + String(SMTP_FROM_ADDRESS) + ">", 250)) { client->stop(); return false; }

  String recipients = String(SMTP_TO_ADDRESS);
  recipients.replace(" ", "");
  int start = 0;
  while (start < (int)recipients.length()) {
    int commaIndex = recipients.indexOf(',', start);
    if (commaIndex == -1) commaIndex = recipients.length();
    String address = recipients.substring(start, commaIndex);
    if (address.length() > 0) {
      if (!sendSmtpCommand(*client, "RCPT TO:<" + address + ">", 250)) { client->stop(); return false; }
    }
    start = commaIndex + 1;
  }

  if (!sendSmtpCommand(*client, "DATA", 354)) { client->stop(); return false; }

  client->printf("From: <%s>\r\n", SMTP_FROM_ADDRESS);
  client->printf("To: %s\r\n", SMTP_TO_ADDRESS);
  client->printf("Subject: %s\r\n", title.c_str());
  client->println("Content-Type: text/plain; charset=\"UTF-8\"\r\n");
  client->println(message);
  client->println(".");

  if (!readSmtpResponse(*client, 250)) { client->stop(); return false; }
  sendSmtpCommand(*client, "QUIT", 221);
  client->stop();

  Serial.println("SMTP notification sent");
  return true;
}

bool sendMeshCoreNotificationWithStatus(const String& title, const String& message) {
  // ESP32-S3 cannot run WiFi and BLE simultaneously
  // Disconnect WiFi, connect BLE, send message, disconnect BLE, reconnect WiFi
  
  Serial.println("Starting MeshCore notification (BLE operation)...");
  
  // Pause monitoring to prevent false positives during network transition
  monitoringPaused = true;
  bleOperationInProgress = true;
  
  // Disconnect WiFi before starting BLE
  disconnectWiFi();
  
  // Create the layered protocol stack on heap to reduce stack usage.
  // Stack overflow can occur when these large objects are combined with
  // deep BLE callback chains during sendTextMessageToChannel.
  BLECentralTransport::Config config;
  config.deviceName = BLE_DEVICE_NAME;
  config.peerName = BLE_PEER_NAME;
  config.pairingPin = BLE_PAIRING_PIN;
  
  BLECentralTransport* transport = new BLECentralTransport(config);
  FrameCodec* codec = new FrameCodec(*transport);
  CompanionProtocol* protocol = new CompanionProtocol(*transport, *codec);
  
  bool success = false;
  
  // Initialize and connect
  if (transport->init()) {
    if (transport->connect()) {
      // Start session
      if (protocol->startSession("ESP32-Uptime")) {
        // Find the configured channel
        uint8_t channelIdx;
        if (protocol->findChannelByName(BLE_MESH_CHANNEL_NAME, channelIdx)) {
          // Build the message: combine title and message
          String fullMessage = title + ": " + message;
          
          // Send using the protocol layer
          if (protocol->sendTextMessageToChannel(channelIdx, fullMessage)) {
            Serial.println("MeshCore notification sent successfully");
            success = true;
          } else {
            Serial.printf("MeshCore notification failed: send error - %s\n", protocol->getLastError().c_str());
          }
        } else {
          Serial.printf("MeshCore notification skipped: channel not found - %s\n", protocol->getLastError().c_str());
        }
      } else {
        Serial.printf("MeshCore notification skipped: session start failed - %s\n", protocol->getLastError().c_str());
      }
    } else {
      Serial.printf("MeshCore notification skipped: not connected - %s\n", transport->getLastError().c_str());
    }
  } else {
    Serial.printf("MeshCore notification skipped: BLE init failed - %s\n", transport->getLastError().c_str());
  }
  
  // Disconnect BLE and deinitialize to free resources
  transport->disconnect();
  transport->deinit();
  
  // Clean up heap-allocated objects
  delete protocol;
  delete codec;
  delete transport;
  
  // Reconnect WiFi
  reconnectWiFi();
  
  // Resume monitoring
  bleOperationInProgress = false;
  monitoringPaused = false;
  
  Serial.println("MeshCore notification operation complete");
  return success;
}

// Notification queue helper functions

int findQueuedNotification(const String& serviceId) {
  for (int i = 0; i < queuedNotificationCount; i++) {
    if (notificationQueue[i].serviceId == serviceId) {
      return i;
    }
  }
  return -1;
}

void removeQueuedNotification(int index) {
  if (index < 0 || index >= queuedNotificationCount) return;
  
  // Shift remaining notifications down
  for (int i = index; i < queuedNotificationCount - 1; i++) {
    notificationQueue[i] = notificationQueue[i + 1];
  }
  queuedNotificationCount--;
  Serial.printf("Removed notification from queue, %d remaining\n", queuedNotificationCount);
}

void queueNotification(const String& serviceId, const String& title, const String& message, 
                       bool isUp, const String& tags, bool ntfyFailed, bool discordFailed, 
                       bool smtpFailed, bool meshFailed) {
  // Check if any channel actually failed
  if (!ntfyFailed && !discordFailed && !smtpFailed && !meshFailed) {
    return;  // Nothing to queue
  }
  
  // Look for existing notification for this service
  int existingIndex = findQueuedNotification(serviceId);
  
  if (existingIndex >= 0) {
    // Update existing notification with latest state
    // This ensures we only keep the most recent state (up or down)
    QueuedNotification& existing = notificationQueue[existingIndex];
    existing.title = title;
    existing.message = message;
    existing.isUp = isUp;
    existing.tags = tags;
    existing.ntfyPending = ntfyFailed;
    existing.discordPending = discordFailed;
    existing.smtpPending = smtpFailed;
    existing.meshPending = meshFailed;
    existing.lastRetry = millis();
    Serial.printf("Updated queued notification for service %s (now %s)\n", 
                  serviceId.c_str(), isUp ? "UP" : "DOWN");
  } else {
    // Add new notification to queue
    if (queuedNotificationCount >= MAX_QUEUED_NOTIFICATIONS) {
      Serial.println("Notification queue full, dropping oldest");
      removeQueuedNotification(0);
    }
    
    QueuedNotification& newNotification = notificationQueue[queuedNotificationCount];
    newNotification.serviceId = serviceId;
    newNotification.title = title;
    newNotification.message = message;
    newNotification.isUp = isUp;
    newNotification.tags = tags;
    newNotification.ntfyPending = ntfyFailed;
    newNotification.discordPending = discordFailed;
    newNotification.smtpPending = smtpFailed;
    newNotification.meshPending = meshFailed;
    newNotification.lastRetry = millis();
    queuedNotificationCount++;
    Serial.printf("Queued notification for service %s (%s), %d in queue\n", 
                  serviceId.c_str(), isUp ? "UP" : "DOWN", queuedNotificationCount);
  }
}

void processNotificationQueue() {
  if (queuedNotificationCount == 0) return;
  
  unsigned long currentTime = millis();
  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  
  // Process each queued notification (WiFi-based channels only)
  for (int i = 0; i < queuedNotificationCount; i++) {
    QueuedNotification& notification = notificationQueue[i];
    
    // Check if enough time has passed since last retry
    if (currentTime - notification.lastRetry < NOTIFICATION_RETRY_INTERVAL) {
      continue;
    }
    
    notification.lastRetry = currentTime;
    
    // Retry WiFi-based notifications only if WiFi is connected
    if (wifiConnected) {
      if (notification.ntfyPending && isNtfyConfigured()) {
        if (sendNtfyNotificationWithStatus(notification.title, notification.message, notification.tags)) {
          notification.ntfyPending = false;
          Serial.printf("Retry: ntfy notification sent for %s\n", notification.serviceId.c_str());
        }
      }
      
      if (notification.discordPending && isDiscordConfigured()) {
        if (sendDiscordNotificationWithStatus(notification.title, notification.message)) {
          notification.discordPending = false;
          Serial.printf("Retry: Discord notification sent for %s\n", notification.serviceId.c_str());
        }
      }
      
      if (notification.smtpPending && isSmtpConfigured()) {
        if (sendSmtpNotificationWithStatus(notification.title, notification.message)) {
          notification.smtpPending = false;
          Serial.printf("Retry: SMTP notification sent for %s\n", notification.serviceId.c_str());
        }
      }
    }
    
    // Check if all pending notifications for this service have been sent
    // (MeshCore is processed separately in processMeshCoreQueue)
    if (!notification.ntfyPending && !notification.discordPending && 
        !notification.smtpPending && !notification.meshPending) {
      Serial.printf("All notifications sent for %s, removing from queue\n", notification.serviceId.c_str());
      removeQueuedNotification(i);
      i--;  // Adjust index since we removed an element
    }
  }
}

void processMeshCoreQueue() {
  // Check if MeshCore is configured and if we have any pending MeshCore notifications
  if (!isMeshCoreConfigured() || bleOperationInProgress) return;
  
  // Check if any notifications have pending MeshCore messages
  bool hasPendingMesh = false;
  for (int i = 0; i < queuedNotificationCount; i++) {
    if (notificationQueue[i].meshPending) {
      hasPendingMesh = true;
      break;
    }
  }
  if (!hasPendingMesh) return;
  
  // Check if enough time has passed since last MeshCore retry (10 minutes)
  unsigned long currentTime = millis();
  if (currentTime - lastMeshCoreRetry < MESHCORE_RETRY_INTERVAL) {
    return;
  }
  
  Serial.println("Processing MeshCore queue (batched BLE operation)...");
  
  // Pause monitoring to prevent false positives during network transition
  monitoringPaused = true;
  bleOperationInProgress = true;
  lastMeshCoreRetry = currentTime;
  
  // Disconnect WiFi before starting BLE
  disconnectWiFi();
  
  // Create the layered protocol stack on heap to reduce stack usage.
  // Stack overflow can occur when these large objects are combined with
  // deep BLE callback chains during sendTextMessageToChannel.
  BLECentralTransport::Config config;
  config.deviceName = BLE_DEVICE_NAME;
  config.peerName = BLE_PEER_NAME;
  config.pairingPin = BLE_PAIRING_PIN;
  
  BLECentralTransport* transport = new BLECentralTransport(config);
  FrameCodec* codec = new FrameCodec(*transport);
  CompanionProtocol* protocol = new CompanionProtocol(*transport, *codec);
  
  bool sessionReady = false;
  uint8_t channelIdx = 0;
  
  // Initialize, connect and prepare session once
  if (transport->init()) {
    if (transport->connect()) {
      if (protocol->startSession("ESP32-Uptime")) {
        if (protocol->findChannelByName(BLE_MESH_CHANNEL_NAME, channelIdx)) {
          sessionReady = true;
          Serial.println("MeshCore session ready, sending queued messages...");
        } else {
          Serial.printf("MeshCore batch: channel not found - %s\n", protocol->getLastError().c_str());
        }
      } else {
        Serial.printf("MeshCore batch: session start failed - %s\n", protocol->getLastError().c_str());
      }
    } else {
      Serial.printf("MeshCore batch: not connected - %s\n", transport->getLastError().c_str());
    }
  } else {
    Serial.printf("MeshCore batch: BLE init failed - %s\n", transport->getLastError().c_str());
  }
  
  // Send all pending MeshCore notifications in this single session
  if (sessionReady) {
    for (int i = 0; i < queuedNotificationCount; i++) {
      QueuedNotification& notification = notificationQueue[i];
      if (notification.meshPending) {
        String fullMessage = notification.title + ": " + notification.message;
        if (protocol->sendTextMessageToChannel(channelIdx, fullMessage)) {
          notification.meshPending = false;
          Serial.printf("Retry: MeshCore notification sent for %s\n", notification.serviceId.c_str());
        } else {
          Serial.printf("MeshCore send failed for %s: %s\n", 
                        notification.serviceId.c_str(), protocol->getLastError().c_str());
        }
        // Small delay between messages to avoid overwhelming the receiver
        delay(100);
      }
    }
  }
  
  // Disconnect BLE and deinitialize to free resources
  transport->disconnect();
  transport->deinit();
  
  // Clean up heap-allocated objects
  delete protocol;
  delete codec;
  delete transport;
  
  // Reconnect WiFi
  reconnectWiFi();
  
  // Resume monitoring
  bleOperationInProgress = false;
  monitoringPaused = false;
  
  Serial.println("MeshCore batch operation complete");
  
  // Clean up any notifications that have all channels sent
  for (int i = 0; i < queuedNotificationCount; i++) {
    QueuedNotification& notification = notificationQueue[i];
    if (!notification.ntfyPending && !notification.discordPending && 
        !notification.smtpPending && !notification.meshPending) {
      Serial.printf("All notifications sent for %s, removing from queue\n", notification.serviceId.c_str());
      removeQueuedNotification(i);
      i--;
    }
  }
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
    obj["rearmCount"] = services[i].rearmCount;
    // SNMP-specific fields
    obj["snmpOid"] = services[i].snmpOid;
    obj["snmpCommunity"] = services[i].snmpCommunity;
    obj["snmpCompareOp"] = (int)services[i].snmpCompareOp;
    obj["snmpExpectedValue"] = services[i].snmpExpectedValue;
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
    services[serviceCount].rearmCount = obj["rearmCount"] | 0;
    // SNMP-specific fields
    services[serviceCount].snmpOid = obj["snmpOid"] | "";
    services[serviceCount].snmpCommunity = obj["snmpCommunity"] | "public";
    services[serviceCount].snmpCompareOp = (SnmpCompareOp)(obj["snmpCompareOp"].as<int>());
    services[serviceCount].snmpExpectedValue = obj["snmpExpectedValue"] | "";
    services[serviceCount].consecutivePasses = 0;
    services[serviceCount].consecutiveFails = 0;
    services[serviceCount].failedChecksSinceAlert = 0;
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
    case TYPE_HTTP_GET: return "http_get";
    case TYPE_PING: return "ping";
    case TYPE_SNMP_GET: return "snmp_get";
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
                            <option value="http_get">HTTP GET</option>
                            <option value="ping">Ping</option>
                            <option value="snmp_get">SNMP GET</option>
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

                <div class="form-group">
                    <label for="rearmCount">Re-arm Alert Count (0 = disabled)</label>
                    <input type="number" id="rearmCount" value="0" required min="0" title="Number of failed checks before re-alerting while service is DOWN. Set to 0 to disable.">
                </div>

                <div class="form-group" id="pathGroup">
                    <label for="servicePath">Path</label>
                    <input type="text" id="servicePath" value="/" placeholder="/">
                </div>

                <div class="form-group" id="responseGroup">
                    <label for="expectedResponse">Expected Response (* for any, regex: prefix for regex)</label>
                    <input type="text" id="expectedResponse" value="*" placeholder="*" title="Use * for any response, plain text for substring match, or regex:pattern for regex matching (e.g., regex:status.*ok)">
                </div>

                <div class="form-group hidden" id="snmpOidGroup">
                    <label for="snmpOid">SNMP OID</label>
                    <input type="text" id="snmpOid" value="" placeholder="1.3.6.1.2.1.1.1.0">
                </div>

                <div class="form-group hidden" id="snmpCommunityGroup">
                    <label for="snmpCommunity">SNMP Community String</label>
                    <input type="text" id="snmpCommunity" value="public" placeholder="public">
                </div>

                <div class="form-row hidden" id="snmpCompareGroup">
                    <div class="form-group">
                        <label for="snmpCompareOp">Comparison Operator</label>
                        <select id="snmpCompareOp">
                            <option value="=">=  (Equal)</option>
                            <option value="<>"><>  (Not Equal)</option>
                            <option value="<"><  (Less Than)</option>
                            <option value="<="><= (Less or Equal)</option>
                            <option value=">">  (Greater Than)</option>
                            <option value=">=">>= (Greater or Equal)</option>
                        </select>
                    </div>

                    <div class="form-group">
                        <label for="snmpExpectedValue">Expected Value</label>
                        <input type="text" id="snmpExpectedValue" value="" placeholder="Expected value">
                    </div>
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
            const snmpOidGroup = document.getElementById('snmpOidGroup');
            const snmpCommunityGroup = document.getElementById('snmpCommunityGroup');
            const snmpCompareGroup = document.getElementById('snmpCompareGroup');

            if (type === 'ping') {
                pathGroup.classList.add('hidden');
                responseGroup.classList.add('hidden');
                snmpOidGroup.classList.add('hidden');
                snmpCommunityGroup.classList.add('hidden');
                snmpCompareGroup.classList.add('hidden');
            } else if (type === 'snmp_get') {
                pathGroup.classList.add('hidden');
                responseGroup.classList.add('hidden');
                snmpOidGroup.classList.remove('hidden');
                snmpCommunityGroup.classList.remove('hidden');
                snmpCompareGroup.classList.remove('hidden');
                portInput.value = 161;
            } else {
                pathGroup.classList.remove('hidden');
                snmpOidGroup.classList.add('hidden');
                snmpCommunityGroup.classList.add('hidden');
                snmpCompareGroup.classList.add('hidden');

                if (type === 'http_get') {
                    responseGroup.classList.remove('hidden');
                } else {
                    responseGroup.classList.add('hidden');
                }

                // Set default port for HTTP-based service types
                portInput.value = 80;
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
                failThreshold: parseInt(document.getElementById('failThreshold').value),
                rearmCount: parseInt(document.getElementById('rearmCount').value),
                snmpOid: document.getElementById('snmpOid').value,
                snmpCommunity: document.getElementById('snmpCommunity').value,
                snmpCompareOp: document.getElementById('snmpCompareOp').value,
                snmpExpectedValue: document.getElementById('snmpExpectedValue').value
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

                const rearmInfo = service.rearmCount > 0 
                    ? `${service.rearmCount} (${service.failedChecksSinceAlert} since last alert)` 
                    : 'disabled';

                // Build SNMP info section if applicable
                let snmpInfo = '';
                if (service.type === 'snmp_get' && service.snmpOid) {
                    snmpInfo = `
                        <div class="service-info">
                            <strong>OID:</strong> ${service.snmpOid}
                        </div>
                        <div class="service-info">
                            <strong>Community:</strong> ${service.snmpCommunity}
                        </div>
                        <div class="service-info">
                            <strong>Check:</strong> value ${service.snmpCompareOp} ${service.snmpExpectedValue}
                        </div>
                    `;
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
                        ${service.path && service.type !== 'ping' && service.type !== 'snmp_get' ? `
                        <div class="service-info">
                            <strong>Path:</strong> ${service.path}
                        </div>
                        ` : ''}
                        ${snmpInfo}
                        <div class="service-info">
                            <strong>Check Interval:</strong> ${service.checkInterval}s
                        </div>
                        <div class="service-info">
                            <strong>Thresholds:</strong> ${service.failThreshold} fail / ${service.passThreshold} pass
                        </div>
                        <div class="service-info">
                            <strong>Re-arm Alert:</strong> ${rearmInfo}
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
