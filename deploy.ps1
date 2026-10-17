# language: powershell, file: deploy.ps1
# *LILBIG4DEV ESP32-S3 — Full Auto Deploy (Compile -> Flash -> GitHub)*
# *usage: .\deploy.ps1 "commit message"*
# *requires: Arduino IDE installed (provides arduino-cli), Git, gh*

param(
    [string]$Msg = "update firmware $(Get-Date -Format 'yyyy-MM-dd HH:mm')",
    [string]$Port = "COM10"
)

$ErrorActionPreference = "Stop"

# ============ CONFIG ============
$GHUser     = "InfinityTeq"
$GHRepoName = "esp32s3_kit"
$Branch     = "main"
$RemoteUrl  = "https://github.com/$GHUser/$GHRepoName.git"
$FQBN       = "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=custom,PSRAM=opi,CPUFreq=240,UploadSpeed=921600"

# Paths to the IDE's bundled tools
$IDEPath    = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources"
$CLI        = "$IDEPath\arduino-cli.exe"
$CONFIG     = "$env:LOCALAPPDATA\Arduino15\arduino-cli.yaml"
# ================================

function Say  { Write-Host "[deploy] $args" -ForegroundColor Cyan }
function Ok   { Write-Host "   ok    $args" -ForegroundColor Green }
function Warn { Write-Host "   !!    $args" -ForegroundColor Yellow }
function Die  { Write-Host "[died]   $args" -ForegroundColor Red; exit 1 }

Set-Location $PSScriptRoot

# ---------------------------------------------------------------
# 0. Ensure Core Files Exist (Auto-heal missing files)
# ---------------------------------------------------------------
if (-not (Test-Path "partitions.csv")) {
    Say "Generating partitions.csv..."
    $partitions = "# Name,   Type, SubType, Offset,   Size,     Flags`nnvs,      data, nvs,     0x9000,   0x5000,`notadata,  data, ota,     0xe000,   0x2000,`napp0,     app,  ota_0,   0x10000,  0x3C0000,`nspiffs,   data, spiffs,  0x3D0000, 0x420000,`n"
    [System.IO.File]::WriteAllText("$PWD\partitions.csv", $partitions)
}

if (-not (Test-Path $CLI)) {
    Warn "Arduino CLI not found at: $CLI"
    Die "Please ensure Arduino IDE is installed in the default location."
}

# ---------------------------------------------------------------
# 1. Compile Firmware
# ---------------------------------------------------------------
Say "Compiling firmware (this might take a minute)..."
& $CLI compile --config-file "$CONFIG" --fqbn $FQBN .
if ($LASTEXITCODE -ne 0) { Die "Compilation failed." }
Ok "Compile successful."

# ---------------------------------------------------------------
# 2. Flash to Board
# ---------------------------------------------------------------
Say "Flashing to $Port..."
& $CLI upload --config-file "$CONFIG" -p $Port --fqbn $FQBN .
if ($LASTEXITCODE -ne 0) {
    Warn "Upload failed. Make sure the board is plugged in."
    Warn "Try forcing bootloader mode: Hold BOOT, tap RST, release BOOT."
    Die "Upload failed."
}
Ok "Flash successful."

# ---------------------------------------------------------------
# 3. Git Commit & Push
# ---------------------------------------------------------------
Say "Staging Git changes..."
git add -A
$diff = git diff --cached --name-only

if ([string]::IsNullOrWhiteSpace($diff)) {
    Warn "Nothing staged - skipping commit."
} else {
    if ($diff -match '\.(apk|key|pem)$') {
        Die "Staged file looks like a payload/secret - unstage it."
    }
    git commit -q -m $Msg
    Ok "Committed $(git rev-parse --short HEAD)"
}

# Setup remote if fresh clone
$currentRemote = git remote get-url origin 2>$null
if ($LASTEXITCODE -ne 0) {
    git remote add origin $RemoteUrl
} elseif ($currentRemote -ne $RemoteUrl) {
    git remote set-url origin $RemoteUrl
}

# Ensure repo exists on GitHub
if (Get-Command gh -ErrorAction SilentlyContinue) {
    gh repo view "$GHUser/$GHRepoName" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) {
        gh repo create "$GHUser/$GHRepoName" --private --source=. --remote=origin
    }
}

Say "Pushing to GitHub..."
$upstream = git rev-parse --abbrev-ref --symbolic-full-name "@{u}" 2>$null
if ($LASTEXITCODE -eq 0) {
    git push -q origin $Branch
} else {
    git push -q -u origin $Branch
}

Ok "Deployed successfully -> https://github.com/$GHUser/$GHRepoName"