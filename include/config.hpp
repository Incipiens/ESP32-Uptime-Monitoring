#pragma once

#include <cstdint>

// Wi-Fi credentials
extern const char* WIFI_SSID;
extern const char* WIFI_PASSWORD;

// Web authentication (Basic Auth)
extern const char* WEB_AUTH_USERNAME;
extern const char* WEB_AUTH_PASSWORD;

// BLE configuration
extern const char* BLE_DEVICE_NAME;
extern const char* BLE_PEER_NAME;
extern const uint32_t BLE_PAIRING_PIN;
extern const char* BLE_MESH_CHANNEL_NAME;

// ntfy configuration
extern const char* NTFY_SERVER;
extern const char* NTFY_TOPIC;
extern const char* NTFY_ACCESS_TOKEN;
extern const char* NTFY_USERNAME;
extern const char* NTFY_PASSWORD;

// Discord configuration
extern const char* DISCORD_WEBHOOK_URL;

// SMTP configuration
extern const char* SMTP_SERVER;
extern const int SMTP_PORT;
extern const bool SMTP_USE_TLS;
extern const char* SMTP_USERNAME;
extern const char* SMTP_PASSWORD;
extern const char* SMTP_FROM_ADDRESS;
extern const char* SMTP_TO_ADDRESS;
