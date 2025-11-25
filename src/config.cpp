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

#ifndef WIFI_SSID_VALUE
#define WIFI_SSID_VALUE "xxx"
#endif

#ifndef WIFI_PASSWORD_VALUE
#define WIFI_PASSWORD_VALUE "xxx"
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

const char* WIFI_SSID = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD = WIFI_PASSWORD_VALUE;

const char* NTFY_SERVER = NTFY_SERVER_VALUE;
const char* NTFY_TOPIC = NTFY_TOPIC_VALUE;
const char* NTFY_ACCESS_TOKEN = NTFY_ACCESS_TOKEN_VALUE;
const char* NTFY_USERNAME = NTFY_USERNAME_VALUE;
const char* NTFY_PASSWORD = NTFY_PASSWORD_VALUE;
