# ESP32 Uptime Monitor

A basic uptime monitor written for the ESP32. It was written for an XDA article.

It serves as a framework to monitor services where support can be hardcoded as a type, making it expandable. Users can also add their own GET requests and ping requests, so that it can support any service. You can't set any authorization, which is a limitation of this application, though could be added in the form processing.

## Features

- **Home Assistant** monitoring
- **Jellyfin** server monitoring
- **HTTP GET** requests with expected response validation
- **Ping** monitoring
- Optional **ntfy offline notifications** when services go down
- Web-based UI for adding and managing services
- Persistent storage using LittleFS

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
3. Flash the firmware again. The device will publish a message to the topic whenever it detects that a service transitioned to a down state.

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
