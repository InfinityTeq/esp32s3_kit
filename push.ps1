param(
    [string]$Msg = "update firmware $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
)
$ErrorActionPreference = "Stop"
$GHUser     = "LILBIG4DEV"
$GHRepoName = "esp32s3_kit"
$Branch     = "main"
$RemoteUrl  = "https://github.com/LILBIG4DEV/esp32s3_kit.git"
function Say  { Write-Host "[push] $args" -ForegroundColor Cyan }
function Ok   { Write-Host "  ok  $args" -ForegroundColor Green }
function Warn { Write-Host "  !!  $args" -ForegroundColor Yellow }
function Die  { Write-Host "[died] $args" -ForegroundColor Red; exit 1 }
Set-Location $PSScriptRoot
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Die "git not installed." }
if (-not (Test-Path ".git")) {
    git init -q
    git branch -M $Branch 2>$null
}
$currentRemote = git remote get-url origin 2>$null
if ($LASTEXITCODE -eq 0) {
    if ($currentRemote -ne $RemoteUrl) { git remote set-url origin $RemoteUrl }
} else {
    git remote add origin $RemoteUrl
}
git add -A
$diff = git diff --cached --name-only
if ([string]::IsNullOrWhiteSpace($diff)) {
    Warn "nothing staged"
} else {
    if ($diff -match '\.(apk|key|pem)$') { Die "staged file is payload/secret" }
    git commit -q -m $Msg
    Ok "committed $(git rev-parse --short HEAD)"
}
if (Get-Command gh -ErrorAction SilentlyContinue) {
    gh repo view "$GHUser/$GHRepoName" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { gh repo create "$GHUser/$GHRepoName" --private --source=. --remote=origin }
}
git push -q -u origin $Branch
Ok "done -> https://github.com/$GHUser/$GHRepoName"