# esp32s3_kit - LILBIG4DEV ESP32-S3

A 35-slot offensive toolkit for the ESP32-S3. Wi-Fi recon and injection, BLE spam/scan/beacon, HID keyboard + mouse + DuckyScript, IR blast/recv, RF stubs, captive-portal delivery, pure-HID Android dropper, and an HTTP upload slot to push large payloads directly into LittleFS.

![LILBIG4DEV Dashboard](assets/dashboard.png)

## Hardware
- MCU: ESP32-S3 (QFN56 rev v0.2), dual-core @ 240 MHz, 8 MB PSRAM
- Flash: 8 MB
- Display: ST7789 TFT (135x240) via SPI
- USB: Native USB-Serial/JTAG

### Pin Map
```text
BTN_PIN 0 (short = next, long >=800ms = prev)
LED_PIN 48
TFT_CS 10 / TFT_DC 9 / TFT_RST 8 / TFT_BL 7
IR_TX_PIN 4 / IR_RX_PIN 5
CC1101_CS 21 / NRF24_CS 14
GPS_RX 16 / GPS_TX 17 (UART2)
```

## Setup Guide (Arduino IDE)

### 1. Required Libraries
You MUST install these 5 libraries via Sketch -> Include Library -> Manage Libraries:
1. Adafruit GFX Library
2. Adafruit ST7735 and ST7789 Library
3. Adafruit BusIO
4. ArduinoJson
5. IRremoteESP8266

(Note: WiFi, BLEDevice, USBHID, WebServer, DNSServer, and LittleFS are built into the ESP32 core. Do not install separate copies.)

### 2. Select the Board
Go to Tools and set exactly these parameters:
- Board: ESP32S3 Dev Module
- USB CDC On Boot: Enabled
- CPU Frequency: 240MHz
- Flash Mode: QIO 80MHz
- Flash Size: 8MB (64Mb)
- Partition Scheme: Custom (Requires partitions.csv in the sketch folder)
- PSRAM: OPI PSRAM
- Upload Speed: 921600

### 3. The Custom Partition
To fit a 3.5MB APK in the filesystem alongside the 1.4MB firmware, this repo uses a custom partition table. Ensure partitions.csv is in the same folder as esp32s3_kit.ino:

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x3C0000,
spiffs,   data, spiffs,  0x3D0000, 0x420000,
```

(If "Custom" does not appear in the Tools menu, ensure the file is saved with LF line endings and UTF-8 encoding, then restart the IDE).

### 4. Compiling & Flashing
1. Open esp32s3_kit.ino in the Arduino IDE.
2. Click the Upload arrow.
3. If it fails to connect (No serial data received), force Download Mode on the board:
   - Hold the BOOT button.
   - Tap the RST button once.
   - Release the BOOT button.
   - Click Upload again.

## First Boot
Open the Serial Monitor at 115200 baud. You will see:
```text
[*] payload url: http://192.168.4.1/omg.apk
[*] dashboard http://192.168.4.1
[*] AP: LILBIG4DEV-S3  pass: letmecook123
[*] LILBIG4DEV ESP32-S3 ready
```

1. Join the Wi-Fi network LILBIG4DEV-S3 (password: letmecook123).
2. Open a browser and go to http://192.168.4.1.

## Uploading an APK (APKUP)
Because the dashboard and APKUP both bind to the same IP, they run sequentially.

1. Format the filesystem (One-time setup): In the serial console, type panic wipefs. The board will reboot and format the 4MB LittleFS partition.
2. Enter APKUP mode: Type slot 34 in the serial console.
   > slot 34
   [*] AP now: APKUP (open) ip 192.168.4.1

3. Upload the file: Your laptop must be connected to the APKUP Wi-Fi network. Open PowerShell or Git Bash in the folder containing your .apk, and run:
   curl -F 'file=@omg.apk' http://192.168.4.1/upload
   (The serial monitor will show: [*] upload done 3535070 bytes -> /omg.apk)

4. Restore the Dashboard: Type apkup stop in the serial console. The LILBIG4DEV-S3 Wi-Fi network will return.
5. Fire the payload: Type dropserv to host the APK locally via captive portal, or android go to use HID typing.

## Pushing updates to GitHub
This repo includes push.ps1 for quick Windows commits.

```powershell
.\push.ps1 "init: ESP32-S3 firmware"
.\push.ps1 "added new slot"
```

---

## One-Shot Deployment (Windows)

If you have the Arduino IDE installed, you can compile, flash, and push to GitHub all in one command without ever opening the IDE GUI. The `deploy.ps1` script handles it all.

Ensure the script is executable and run it from your PowerShell terminal:

```powershell
.\deploy.ps1 "your commit message"
```

*If your ESP32-S3 is connected to a port other than COM10, pass it as the second argument:*
```powershell
.\deploy.ps1 "your commit message" "COM11"
```

**What it does:**
1. Auto-generates `partitions.csv` if it's missing.
2. Compiles the firmware using the bundled Arduino CLI.
3. Flashes it to the board (reminds you to hold BOOT + RST if the USB drops).
4. Commits the source code and pushes it to GitHub.
