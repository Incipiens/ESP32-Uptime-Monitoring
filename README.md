# ESP32 Uptime Monitor

A basic uptime monitor written for the ESP32. It was written for an XDA article.

It serves as a framework to monitor services where support can be hardcoded as a type, making it expandable. Users can also add their own GET requests and ping requests, so that it can support any service.

## Features

- **Home Assistant** monitoring
- **Jellyfin** server monitoring
- **HTTP GET** requests with expected response validation
- **Ping** monitoring
- **Pass/Fail Thresholds** - Configure how many consecutive successes or failures are required before changing a service's status and sending notifications
- Optional **ntfy offline notifications** when services go down
- Optional **Discord webhook notifications** for service up/down events
- Optional **SMTP email notifications** for service up/down events
- Web-based UI for adding and managing services
- Persistent storage using LittleFS
- **Export/Import** monitor configurations for backup and restore

## Prerequisites

Before deploying the code to your ESP32 board, you need to have PlatformIO installed. There are two main ways to use PlatformIO:

### Option 1: VS Code with PlatformIO Extension (Recommended)

1. Install [Visual Studio Code](https://code.visualstudio.com/)
2. Open VS Code and go to the Extensions view (Ctrl+Shift+X / Cmd+Shift+X)
3. Search for "PlatformIO IDE" and install it
4. Restart VS Code when prompted

### Option 2: PlatformIO Core (CLI)

Install PlatformIO Core using pip:

```bash
pip install platformio
```

## Configuration

Before deploying, you need to configure your WiFi credentials:

1. Open `src/main.cpp`
2. Find the following lines near the top of the file:
   ```cpp
   const char* WIFI_SSID = "xxx";
   const char* WIFI_PASSWORD = "xxx";
   ```
3. Replace `"xxx"` with your actual WiFi network name and password:
   ```cpp
   const char* WIFI_SSID = "YourNetworkName";
   const char* WIFI_PASSWORD = "YourPassword";
   ```

### Protecting the web interface

Optional HTTP Basic Authentication can be enabled to prevent unauthenticated users from adding, importing, or deleting services.

Add the following values to `src/config.cpp` (or provide them via PlatformIO build flags) to require credentials when accessing the interface or modifying services:

```cpp
const char* WEB_AUTH_USERNAME = "admin";
const char* WEB_AUTH_PASSWORD = "strong-password";
```

### Pairing with MeshCore over BLE

The firmware now acts as a BLE **client** that connects to the Heltec T114 (MeshCore) using its advertised name and pairing PIN, then writes alert messages to the MeshCore inbox characteristic.

1. **Set the MeshCore peer name, PIN, and channel**
   - Edit `src/config.cpp` or provide build flags so the ESP32 knows how to reach the T114:
     ```cpp
     const char* BLE_PEER_NAME = "Heltec-T114";   // the T114's advertised name
     const uint32_t BLE_PAIRING_PIN = 123456;      // the MeshCore pairing PIN
     const char* BLE_MESH_CHANNEL_NAME = "alerts"; // channel to use on the T114 (must already exist on the device)
     // Optional: local name shown by the ESP32 itself
     const char* BLE_DEVICE_NAME = "ESP32-Uptime";
     ```
     ```ini
     ; platformio.ini
     -DBLE_PEER_NAME_VALUE=\"Heltec-T114\"
     -DBLE_PAIRING_PIN_VALUE=123456
     -DBLE_MESH_CHANNEL_NAME_VALUE=\"alerts\"
     -DBLE_DEVICE_NAME_VALUE=\"MyMeshBridge\"
     ```
   
   **Note:** The channel must be pre-configured on the MeshCore device before use. The ESP32 will not create or modify the channel.

2. **Flash the firmware and watch the serial monitor**
   - On boot the ESP32 scans for the MeshCore name. Logs will show `MeshCore peer found, attempting connection...` and `Connected to MeshCore peer...` once the link is up. If the peer is missing or the PIN is wrong, the error message appears in `/api/mesh/status`.

3. **Allow the ESP32 to initiate the connection**
   - Put the T114/MeshCore radio in pairing mode so it advertises with the configured name and accepts the PIN above. The ESP32 will pair, bond, and stay connected; the T114 does **not** subscribe to the ESP32.

4. **Triggering alerts to MeshCore**
   - The ESP32 acts as a client: it scans for the T114, pairs, and then writes alerts into the configured channel over the MeshCore characteristic. The channel must already exist on the MeshCore device.
   - You can also send ad-hoc messages via the web API (the ESP32 will connect first if needed):
     ```bash
     curl -X POST http://<device-ip>/api/mesh/send \
       -u "<user>:<pass>" \
       -H "Content-Type: application/json" \
       -d '{"title":"Test","message":"Hello Mesh"}'
     ```
     Replace `<user>`/`<pass>` with your Basic Auth credentials (if enabled) and `<device-ip>` with the ESP32 IP address. The endpoint returns an error if the T114 cannot be found or paired.

### Enabling ntfy notifications

Set an ntfy topic to receive alerts whenever a monitored service goes offline. Optional bearer or basic authentication is also supported for secured ntfy servers.

1. In `src/main.cpp`, locate the ntfy configuration block:
   ```cpp
   const char* NTFY_SERVER = "https://ntfy.sh";
   const char* NTFY_TOPIC = "";
   const char* NTFY_ACCESS_TOKEN = "";
   const char* NTFY_USERNAME = "";
   const char* NTFY_PASSWORD = "";
   ```
2. Replace `NTFY_TOPIC` with your topic name (for self-hosted ntfy, update `NTFY_SERVER` as well). Provide authentication if required by your server:
   ```cpp
   const char* NTFY_SERVER = "https://ntfy.sh";
   const char* NTFY_TOPIC = "esp32-uptime";
   // use either a bearer token or basic auth credentials
   const char* NTFY_ACCESS_TOKEN = "my-token";
   // const char* NTFY_USERNAME = "user";
   // const char* NTFY_PASSWORD = "pass";
   ```
   3. Flash the firmware again. The device will publish a message to the topic whenever it detects that a service transitioned to a down state, and another when the service comes back online.

### Enabling Discord notifications

Send alerts to a Discord channel by configuring a webhook URL.

1. Create a webhook in your target Discord channel (Channel Settings → Integrations → Webhooks) and copy the webhook URL.
2. In `src/config.cpp`, set the webhook value:
   ```cpp
   const char* DISCORD_WEBHOOK_URL = "";
   ```
   Replace the empty string with your webhook URL. You can also provide this at build time with a PlatformIO flag:
   ```ini
   -DDISCORD_WEBHOOK_URL_VALUE=\"https://discord.com/api/webhooks/...\"
   ```
3. Rebuild and flash the firmware. A message will be sent to the webhook whenever a service changes state between up and down.

### Enabling SMTP notifications

Configure an SMTP relay to receive email alerts for uptime changes.

1. In `src/config.cpp`, set your SMTP details:
   ```cpp
   const char* SMTP_SERVER = "";
   const int SMTP_PORT = 587;
   const bool SMTP_USE_TLS = true;
   const char* SMTP_USERNAME = "";
   const char* SMTP_PASSWORD = "";
   const char* SMTP_FROM_ADDRESS = "";
   const char* SMTP_TO_ADDRESS = ""; // Comma-separated list is supported
   ```
2. For most servers you can provide the same values through PlatformIO build flags instead of editing the file directly:
   ```ini
   -DSMTP_SERVER_VALUE=\"smtp.example.com\"
   -DSMTP_PORT_VALUE=587
   -DSMTP_USE_TLS_VALUE=true
   -DSMTP_USERNAME_VALUE=\"user@example.com\"
   -DSMTP_PASSWORD_VALUE=\"password\"
   -DSMTP_FROM_ADDRESS_VALUE=\"monitor@example.com\"
   -DSMTP_TO_ADDRESS_VALUE=\"alerts@example.com\"
   ```
3. Rebuild and flash the firmware. The device will send an email whenever a service changes between up and down states. Multiple recipients can be provided as a comma-separated list.

## Deploying to ESP32

### Connect Your ESP32 Board

1. Connect your ESP32 board to your computer using a USB cable (this project is configured for ESP32-S3-DevKitC-1, but other ESP32 boards may work with minor configuration changes)
2. The board should be automatically detected by your system
3. On Linux, you may need to add your user to the `dialout` group:
   ```bash
   sudo usermod -a -G dialout $USER
   ```
   Log out and back in for the changes to take effect.

### Using VS Code with PlatformIO IDE

1. Open the project folder in VS Code
2. Wait for PlatformIO to initialize (you'll see a progress indicator in the status bar)
3. Click the PlatformIO icon in the left sidebar
4. Under "Project Tasks" > "esp32-s3-devkitc-1":
   - Click **Build** to compile the firmware
   - Click **Upload** to flash the firmware to your ESP32
   - Click **Monitor** to view serial output

Alternatively, use the keyboard shortcuts:
- **Ctrl+Alt+B** (Cmd+Alt+B on Mac): Build
- **Ctrl+Alt+U** (Cmd+Alt+U on Mac): Upload

### Using PlatformIO CLI

Navigate to the project directory and run:

```bash
# Build the firmware
pio run

# Upload to the ESP32
pio run --target upload

# Monitor serial output
pio device monitor
```

Or combine build and upload in one command:

```bash
pio run --target upload && pio device monitor
```

## Monitoring Serial Output

After uploading, open the serial monitor to view debug information and the IP address of your ESP32:

- **VS Code**: Click "Monitor" in the PlatformIO sidebar
- **CLI**: Run `pio device monitor`

The monitor is configured to run at 115200 baud rate.

You should see output similar to:

```
Starting ESP32 Uptime Monitor...
LittleFS mounted successfully
Connecting to WiFi...
...
WiFi connected!
IP address: 192.168.1.100
Web server started
System ready!
Access web interface at: http://192.168.1.100
```

## Accessing the Web Interface

Once the ESP32 is running and connected to your WiFi:

1. Note the IP address shown in the serial monitor
2. Open a web browser on a device connected to the same network
3. Navigate to `http://<ESP32_IP_ADDRESS>` (e.g., `http://192.168.1.100`)
4. Use the web interface to add and manage monitoring services

## Backup and Restore Monitor Configurations

The web interface provides export and import functionality to backup and restore your monitor configurations. This is useful when:

- Performing firmware updates that may reset the filesystem
- Migrating configurations to a different ESP32 device
- Creating backups before making changes

### Exporting Monitors

1. Click the **Export Monitors** button in the web interface
2. A JSON file (`monitors-backup.json`) will be downloaded containing all your service configurations
3. Store this file safely for future restoration

### Importing Monitors

1. Click the **Import Monitors** button in the web interface
2. Select a previously exported JSON backup file
3. The services will be added to your current configuration
4. A message will indicate how many services were imported

**Note:** Importing adds services to existing ones rather than replacing them. If you want to start fresh, delete existing services before importing.

## Troubleshooting

### Upload Failed

- Ensure the USB cable supports data transfer (not just charging)
- Try a different USB port
- Hold the BOOT button on the ESP32 while initiating upload
- Check that no other application is using the serial port

### WiFi Connection Failed

- Verify your SSID and password are correct
- Ensure your WiFi network is 2.4GHz (ESP32 doesn't support 5GHz)
- Check that the ESP32 is within range of your router

### Serial Monitor Shows Garbage Characters

- Ensure the baud rate is set to 115200
- Try resetting the ESP32 board

## License

See the [LICENSE](LICENSE) file for details.
