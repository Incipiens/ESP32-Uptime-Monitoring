# Copilot Instructions for ESP32 Uptime Monitor

This document provides guidance for AI coding agents working on the ESP32 Uptime Monitor codebase.

## Project Overview

ESP32 Uptime Monitor is a network service monitoring application designed for ESP32-S3 microcontrollers. It provides:

- **Service Monitoring**: Home Assistant, Jellyfin, HTTP GET requests, and ICMP Ping
- **Notification Channels**: ntfy, Discord webhooks, SMTP email, and MeshCore (BLE mesh network)
- **Web Interface**: HTML/CSS/JavaScript UI served directly from the ESP32
- **Persistent Storage**: LittleFS filesystem for configuration persistence
- **Export/Import**: JSON-based backup and restore of monitor configurations

## Technology Stack

| Component | Technology |
|-----------|------------|
| Platform | ESP32-S3 (Espressif32) |
| Framework | Arduino (PlatformIO) |
| Language | C++ (Arduino style, pre-C++17) |
| Web Server | ESPAsyncWebServer |
| JSON | ArduinoJson v7 |
| Filesystem | LittleFS |
| BLE | ESP-IDF BLE (NimBLE compatible) |

## Directory Structure

```
├── src/
│   ├── main.cpp           # Main application, web server, service monitoring
│   └── config.cpp         # User configuration (WiFi, notifications) - gitignored
├── include/
│   ├── config.hpp         # Configuration declarations
│   └── README             # Include directory documentation
├── lib/
│   └── MeshCore/          # MeshCore BLE protocol implementation
│       └── src/
│           ├── MeshCore.hpp             # Umbrella header
│           ├── IByteTransport.hpp       # Transport interface
│           ├── BLECentralTransport.*    # BLE layer implementation
│           ├── FrameCodec.*             # Frame parsing/building layer
│           └── CompanionProtocol.*      # MeshCore protocol logic
├── test/                  # PlatformIO test directory (currently empty)
├── platformio.ini         # PlatformIO build configuration
└── partitions.csv         # ESP32 partition table
```

## Code Style and Conventions

### C++ Style

- **Arduino-style C++**: Uses Arduino String, Serial.println(), delay(), millis()
- **Pre-C++17 compatibility**: Static constexpr members require ODR-use definitions in .cpp files
- **Raw string literals**: HTML/CSS/JS embedded using `R"rawliteral(...)rawliteral"`
- **Naming conventions**:
  - Constants: `UPPER_SNAKE_CASE` (e.g., `MAX_SERVICES`, `WIFI_SSID`)
  - Member variables: `m_camelCase` (e.g., `m_connected`, `m_rxCallback`)
  - Static members: `s_camelCase` (e.g., `s_instance`)
  - Functions: `camelCase` (e.g., `checkServices()`, `sendNtfyNotification()`)
  - Types/Enums: `PascalCase` (e.g., `ServiceType`, `State`)

### Error Handling

- Use `Serial.println()` / `Serial.printf()` for debug logging
- Store last error in `m_lastError` String member for retrieval via `getLastError()`
- Return boolean success/failure from functions
- Use early returns for validation failures

### Memory Management

- Prefer stack allocation where possible
- Use `new`/`delete` sparingly, primarily for callback objects
- Be mindful of ESP32 heap constraints (~300KB available)
- Use `ESP.getFreeHeap()` to monitor memory usage

## Key Implementation Patterns

### Service Monitoring

Services are stored in a fixed-size array with threshold-based state transitions:

```cpp
struct Service {
  String id;
  String name;
  ServiceType type;
  // ... network config ...
  int passThreshold;      // Consecutive passes needed for UP
  int failThreshold;      // Consecutive fails needed for DOWN
  int consecutivePasses;
  int consecutiveFails;
  bool isUp;
};
```

### Async Web Server

ESPAsyncWebServer handles HTTP requests asynchronously. Body handlers use lambdas:

```cpp
server.on("/api/endpoint", HTTP_POST,
  [](AsyncWebServerRequest *request) {},  // Empty for body handler
  NULL,  // No upload handler
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    // Process body data
  }
);
```

### BLE/WiFi Coexistence

ESP32-S3 cannot run WiFi and BLE simultaneously. The pattern is:

1. Pause monitoring (`monitoringPaused = true`)
2. Disconnect WiFi (`WiFi.disconnect(true); WiFi.mode(WIFI_OFF);`)
3. Initialize and use BLE
4. Deinitialize BLE
5. Reconnect WiFi
6. Resume monitoring

### MeshCore Protocol Stack

Layered architecture (bottom to top):

1. **BLECentralTransport**: BLE connection, Nordic UART Service
2. **FrameCodec**: Frame parsing, `[cmd][payload]` format
3. **CompanionProtocol**: MeshCore state machine, commands, responses

## Configuration

User-specific configuration is in `src/config.cpp` (gitignored). The header `include/config.hpp` declares:

- WiFi credentials (`WIFI_SSID`, `WIFI_PASSWORD`)
- Web authentication (`WEB_AUTH_USERNAME`, `WEB_AUTH_PASSWORD`)
- BLE settings (`BLE_PEER_NAME`, `BLE_PAIRING_PIN`, `BLE_MESH_CHANNEL_NAME`)
- Notification settings (ntfy, Discord, SMTP)

Build flags in `platformio.ini` can override config values:

```ini
build_flags =
    -DWIFI_SSID_VALUE=\"MyNetwork\"
    -DBLE_PEER_NAME_VALUE=\"Heltec-T114\"
```

## Building and Testing

### Prerequisites

- PlatformIO Core (CLI) or VS Code with PlatformIO extension
- USB connection to ESP32-S3 board

### Build Commands

```bash
# Build firmware
pio run

# Upload to ESP32
pio run --target upload

# Monitor serial output (115200 baud)
pio device monitor
```

### Testing Notes

- The `test/` directory uses PlatformIO Unit Testing framework
- Unit tests run on target hardware, not host
- Current test infrastructure is minimal; prefer integration testing on device

## Common Development Tasks

### Adding a New Service Type

1. Add enum value to `ServiceType` in `main.cpp`
2. Implement `check<TypeName>(Service& service)` function
3. Add case to `checkServices()` switch statement
4. Update `getServiceTypeString()` for API/UI
5. Add UI handling in `getWebPage()` JavaScript

### Adding a New Notification Channel

1. Add configuration variables to `config.hpp` and `config.cpp`
2. Implement `is<Channel>Configured()` check
3. Implement `send<Channel>Notification(title, message)` function
4. Call from `sendOfflineNotification()` and `sendOnlineNotification()`

### Adding a New Web API Endpoint

```cpp
server.on("/api/new-endpoint", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (!ensureAuthenticated(request)) return;
  
  JsonDocument doc;
  // ... build response ...
  
  String response;
  serializeJson(doc, response);
  request->send(200, "application/json", response);
});
```

## ESP32-Specific Considerations

### Hardware Constraints

- Flash: 8MB (configured in `platformio.ini`)
- RAM: 512KB SRAM total, ~300KB available for heap after system overhead
- WiFi: 2.4GHz only
- BLE and WiFi cannot operate simultaneously on ESP32-S3

### Partition Table

Custom partition layout in `partitions.csv`:

| Partition | Size | Purpose |
|-----------|------|---------|
| nvs | 20KB | Non-volatile storage |
| app0 | 3MB | Application firmware |
| spiffs | 960KB | LittleFS data storage |
| coredump | 64KB | Crash dumps |

### Common Pitfalls

1. **WiFi timeout on boot**: Ensure 2.4GHz network, check credentials
2. **LittleFS corruption**: Partition may need reformatting after flash layout changes
3. **BLE pairing failures**: Ensure MeshCore device is in pairing mode, verify PIN
4. **Task watchdog timeouts**: Avoid blocking operations in HTTP handlers; use deferred processing pattern

## Security Notes

- WiFi credentials and API tokens stored in `config.cpp` (gitignored)
- Web authentication is optional HTTP Basic Auth
- HTTPS/TLS: ESP32 uses `WiFiClientSecure` with `setInsecure()` (no cert validation for simplicity)
  - **Note**: `setInsecure()` disables certificate validation, which is acceptable for development but should be replaced with proper certificate pinning or CA verification in production environments where security is critical
- Input validation: JSON parsing uses ArduinoJson with size limits

## Dependencies

Managed via `platformio.ini` lib_deps:

| Library | Version | Purpose |
|---------|---------|---------|
| ESPAsyncWebServer | 3.6.0 | Async HTTP server |
| AsyncTCP | 3.3.2 | TCP for async web server |
| ArduinoJson | 7.4.2 | JSON parsing/serialization |
| ESP32Ping | 1.6 | ICMP ping functionality |

The `MeshCore` library in `lib/` is project-local and not published to the PlatformIO registry.
