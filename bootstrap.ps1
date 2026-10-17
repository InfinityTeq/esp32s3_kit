# language: powershell, file: bootstrap.ps1
# *One-shot generator: scaffolds the repo, writes README, partitions, push script, and pushes to GitHub.*
# *usage: .\bootstrap.ps1*

$ErrorActionPreference = "Stop"

# ============ CONFIG ============
$GHUser     = "LILBIG4DEV"
$GHRepoName = "esp32s3_kit"
$Branch     = "main"
$RemoteUrl  = "https://github.com/$GHUser/$GHRepoName.git"
# ================================

function Say  { Write-Host "[setup] $args" -ForegroundColor Cyan }
function Ok   { Write-Host "   ok   $args" -ForegroundColor Green }
function Die  { Write-Host "[died]  $args" -ForegroundColor Red; exit 1 }

Say "Checking prerequisites..."
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Die "Git not installed. winget install Git.Git" }
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { Die "GitHub CLI not installed. winget install GitHub.cli" }

gh auth status 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { 
    Say "Launching GitHub authentication..."
    gh auth login -h github.com -p https -w 
}

# 1. Create folders
if (-not (Test-Path "data")) { New-Item -ItemType Directory -Path "data" | Out-Null }
if (-not (Test-Path "data\.gitkeep")) { New-Item -ItemType File -Path "data\.gitkeep" | Out-Null }

# 2. Write .gitignore (LF line endings)
Say "Generating .gitignore..."
$gitignore = "build/`n*.bin`n*.elf`n*.map`n*.hex`n*.d`n*.o`n*.a`n*.su`n.arduino/`n.cache/`n.vscode/`n.idea/`n*.code-workspace`n.DS_Store`nThumbs.db`ndesktop.ini`nsecrets.h`n*.key`n*.pem`ndata/*.apk`ndata/*.bin`n!data/.gitkeep`n__pycache__/`n*.pyc`n"
[System.IO.File]::WriteAllText("$PWD\.gitignore", $gitignore)

# 3. Write partitions.csv (LF line endings critical)
Say "Generating partitions.csv..."
$partitions = "# Name,   Type, SubType, Offset,   Size,     Flags`nnvs,      data, nvs,     0x9000,   0x5000,`notadata,  data, ota,     0xe000,   0x2000,`napp0,     app,  ota_0,   0x10000,  0x3C0000,`nspiffs,   data, spiffs,  0x3D0000, 0x420000,`n"
[System.IO.File]::WriteAllText("$PWD\partitions.csv", $partitions)

# 4. Write push.ps1
Say "Generating push.ps1..."
$pushScript = @"
param(
    [string]`$Msg = "update firmware `$(Get-Date -Format 'yyyy-MM-dd HH:mm')"
)
`$ErrorActionPreference = "Stop"
`$GHUser     = "$GHUser"
`$GHRepoName = "$GHRepoName"
`$Branch     = "$Branch"
`$RemoteUrl  = "$RemoteUrl"
function Say  { Write-Host "[push] `$args" -ForegroundColor Cyan }
function Ok   { Write-Host "  ok  `$args" -ForegroundColor Green }
function Warn { Write-Host "  !!  `$args" -ForegroundColor Yellow }
function Die  { Write-Host "[died] `$args" -ForegroundColor Red; exit 1 }
Set-Location `$PSScriptRoot
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Die "git not installed." }
if (-not (Test-Path ".git")) {
    git init -q
    git branch -M `$Branch 2>`$null
}
`$currentRemote = git remote get-url origin 2>`$null
if (`$LASTEXITCODE -eq 0) {
    if (`$currentRemote -ne `$RemoteUrl) { git remote set-url origin `$RemoteUrl }
} else {
    git remote add origin `$RemoteUrl
}
git add -A
`$diff = git diff --cached --name-only
if ([string]::IsNullOrWhiteSpace(`$diff)) {
    Warn "nothing staged"
} else {
    if (`$diff -match '\.(apk|key|pem)`$') { Die "staged file is payload/secret" }
    git commit -q -m `$Msg
    Ok "committed `$(git rev-parse --short HEAD)"
}
if (Get-Command gh -ErrorAction SilentlyContinue) {
    gh repo view "`$GHUser/`$GHRepoName" 2>`$null | Out-Null
    if (`$LASTEXITCODE -ne 0) { gh repo create "`$GHUser/`$GHRepoName" --private --source=. --remote=origin }
}
git push -q -u origin `$Branch
Ok "done -> https://github.com/`$GHUser/`$GHRepoName"
"@ -replace "`r`n", "`n"
[System.IO.File]::WriteAllText("$PWD\push.ps1", $pushScript)

# 5. Write README.md
Say "Generating README.md..."
$readme = @'
# esp32s3_kit - LILBIG4DEV ESP32-S3

A 35-slot offensive toolkit for the ESP32-S3. Wi-Fi recon and injection, BLE spam/scan/beacon, HID keyboard + mouse + DuckyScript, IR blast/recv, RF stubs, captive-portal delivery, pure-HID Android dropper, and an HTTP upload slot to push large payloads directly into LittleFS.

## Hardware
- MCU: ESP32-S3 (QFN56 rev v0.2), dual-core @ 240 MHz, 8 MB PSRAM
- Flash: 8 MB
- Display: ST7789 TFT (135x240) via SPI
- USB: Native USB-Serial/JTAG

### Pin Map
BTN_PIN 0 (short = next, long >=800ms = prev)
LED_PIN 48
TFT_CS 10 / TFT_DC 9 / TFT_RST 8 / TFT_BL 7
IR_TX_PIN 4 / IR_RX_PIN 5
CC1101_CS 21 / NRF24_CS 14
GPS_RX 16 / GPS_TX 17 (UART2)

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

# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x3C0000,
spiffs,   data, spiffs,  0x3D0000, 0x420000,

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
[*] payload url: http://192.168.4.1/omg.apk
[*] dashboard http://192.168.4.1
[*] AP: LILBIG4DEV-S3  pass: letmecook123
[*] LILBIG4DEV ESP32-S3 ready

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

.\push.ps1 "init: ESP32-S3 firmware"
.\push.ps1 "added new slot"
'@ -replace "`r`n", "`n"
[System.IO.File]::WriteAllText("$PWD\README.md", $readme)

# 6. Setup git and push
Say "Initializing Git..."
if (-not (Test-Path ".git")) {
    git init -q
    git branch -M $Branch 2>$null
}

git remote remove origin 2>$null | Out-Null
git remote add origin $RemoteUrl

Say "Staging files and pushing to GitHub..."
git add -A
git commit -q -m "init: generated firmware setup and docs" 2>$null

gh repo view "$GHUser/$GHRepoName" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Say "Creating GitHub repo $GHUser/$GHRepoName"
    gh repo create "$GHUser/$GHRepoName" --private --source=. --remote=origin
}

git push -q -u origin $Branch

Ok "ALL DONE. Repo is live at https://github.com/$GHUser/$GHRepoName"