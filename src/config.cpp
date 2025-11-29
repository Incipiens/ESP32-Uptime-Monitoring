#include "config.hpp"

// Default values can be overridden at build time using PlatformIO build flags, e.g.:
// build_flags =
//   -DWIFI_SSID_VALUE=\"your-ssid\"
//   -DWIFI_PASSWORD_VALUE=\"your-password\"
//   -DNTFY_SERVER_VALUE=\"https://ntfy.sh\"
//   -DNTFY_TOPIC_VALUE=\"esp32-uptime\"
//   -DNTFY_ACCESS_TOKEN_VALUE=\"token\"
//   -DNTFY_USERNAME_VALUE=\"user\"
//   -DNTFY_PASSWORD_VALUE=\"pass\"
//   -DDISCORD_WEBHOOK_URL_VALUE=\"https://discord.com/api/webhooks/...\"

#ifndef WIFI_SSID_VALUE
#define WIFI_SSID_VALUE "xxx"
#endif

#ifndef WIFI_PASSWORD_VALUE
#define WIFI_PASSWORD_VALUE "xxx"
#endif

#ifndef WEB_AUTH_USERNAME_VALUE
#define WEB_AUTH_USERNAME_VALUE ""
#endif

#ifndef WEB_AUTH_PASSWORD_VALUE
#define WEB_AUTH_PASSWORD_VALUE ""
#endif

#ifndef BLE_DEVICE_NAME_VALUE
#define BLE_DEVICE_NAME_VALUE "ESP32-Uptime"
#endif

#ifndef BLE_PEER_NAME_VALUE
#define BLE_PEER_NAME_VALUE "Heltec-T114"
#endif

#ifndef BLE_PAIRING_PIN_VALUE
#define BLE_PAIRING_PIN_VALUE 123456
#endif

#ifndef BLE_MESH_CHANNEL_NAME_VALUE
#define BLE_MESH_CHANNEL_NAME_VALUE "alerts"
#endif

#ifndef NTFY_SERVER_VALUE
#define NTFY_SERVER_VALUE "https://ntfy.sh"
#endif

#ifndef NTFY_TOPIC_VALUE
#define NTFY_TOPIC_VALUE ""
#endif

#ifndef NTFY_ACCESS_TOKEN_VALUE
#define NTFY_ACCESS_TOKEN_VALUE ""
#endif

#ifndef NTFY_USERNAME_VALUE
#define NTFY_USERNAME_VALUE ""
#endif

#ifndef NTFY_PASSWORD_VALUE
#define NTFY_PASSWORD_VALUE ""
#endif

#ifndef DISCORD_WEBHOOK_URL_VALUE
#define DISCORD_WEBHOOK_URL_VALUE ""
#endif

#ifndef SMTP_SERVER_VALUE
#define SMTP_SERVER_VALUE ""
#endif

#ifndef SMTP_PORT_VALUE
#define SMTP_PORT_VALUE 587
#endif

#ifndef SMTP_USE_TLS_VALUE
#define SMTP_USE_TLS_VALUE true
#endif

#ifndef SMTP_USERNAME_VALUE
#define SMTP_USERNAME_VALUE ""
#endif

#ifndef SMTP_PASSWORD_VALUE
#define SMTP_PASSWORD_VALUE ""
#endif

#ifndef SMTP_FROM_ADDRESS_VALUE
#define SMTP_FROM_ADDRESS_VALUE ""
#endif

#ifndef SMTP_TO_ADDRESS_VALUE
#define SMTP_TO_ADDRESS_VALUE ""
#endif

const char* WIFI_SSID = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD = WIFI_PASSWORD_VALUE;

const char* WEB_AUTH_USERNAME = WEB_AUTH_USERNAME_VALUE;
const char* WEB_AUTH_PASSWORD = WEB_AUTH_PASSWORD_VALUE;

const char* BLE_DEVICE_NAME = BLE_DEVICE_NAME_VALUE;
const char* BLE_PEER_NAME = BLE_PEER_NAME_VALUE;
const uint32_t BLE_PAIRING_PIN = BLE_PAIRING_PIN_VALUE;
const char* BLE_MESH_CHANNEL_NAME = BLE_MESH_CHANNEL_NAME_VALUE;

const char* NTFY_SERVER = NTFY_SERVER_VALUE;
const char* NTFY_TOPIC = NTFY_TOPIC_VALUE;
const char* NTFY_ACCESS_TOKEN = NTFY_ACCESS_TOKEN_VALUE;
const char* NTFY_USERNAME = NTFY_USERNAME_VALUE;
const char* NTFY_PASSWORD = NTFY_PASSWORD_VALUE;

const char* DISCORD_WEBHOOK_URL = DISCORD_WEBHOOK_URL_VALUE;

const char* SMTP_SERVER = SMTP_SERVER_VALUE;
const int SMTP_PORT = SMTP_PORT_VALUE;
const bool SMTP_USE_TLS = SMTP_USE_TLS_VALUE;
const char* SMTP_USERNAME = SMTP_USERNAME_VALUE;
const char* SMTP_PASSWORD = SMTP_PASSWORD_VALUE;
const char* SMTP_FROM_ADDRESS = SMTP_FROM_ADDRESS_VALUE;
const char* SMTP_TO_ADDRESS = SMTP_TO_ADDRESS_VALUE;
