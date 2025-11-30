#include "config.hpp"

// Wi-Fi credentials
const char* WIFI_SSID = "test_network";
const char* WIFI_PASSWORD = "test_password";

// Web authentication (Basic Auth)
const char* WEB_AUTH_USERNAME = "";
const char* WEB_AUTH_PASSWORD = "";

// BLE configuration
const char* BLE_DEVICE_NAME = "ESP32-Uptime";
const char* BLE_PEER_NAME = "";
const uint32_t BLE_PAIRING_PIN = 123456;
const char* BLE_MESH_CHANNEL_NAME = "";

// ntfy configuration
const char* NTFY_SERVER = "";
const char* NTFY_TOPIC = "";
const char* NTFY_ACCESS_TOKEN = "";
const char* NTFY_USERNAME = "";
const char* NTFY_PASSWORD = "";

// Discord configuration
const char* DISCORD_WEBHOOK_URL = "";

// SMTP configuration
const char* SMTP_SERVER = "";
const int SMTP_PORT = 587;
const bool SMTP_USE_TLS = true;
const char* SMTP_USERNAME = "";
const char* SMTP_PASSWORD = "";
const char* SMTP_FROM_ADDRESS = "";
const char* SMTP_TO_ADDRESS = "";

// Boot notification configuration
const bool BOOT_NOTIFICATION_ENABLED = false;
