# language: powershell, file: push.ps1
# *Complete GitHub push script for Windows.*
# *usage: .\push.ps1 "commit message"*

param(
    [string]$Msg = "update firmware $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
)

$ErrorActionPreference = "Stop"

# ============ CONFIG ============
$GHUser     = "InfinityTeq"
$GHRepoName = "esp32s3_kit"
$Branch     = "main"
$RemoteUrl  = "https://github.com/$GHUser/$GHRepoName.git"
# ================================

function Say  { Write-Host "[push] $args" -ForegroundColor Cyan }
function Ok   { Write-Host "  ok  $args" -ForegroundColor Green }
function Warn { Write-Host "  !!  $args" -ForegroundColor Yellow }
function Die  { Write-Host "[died] $args" -ForegroundColor Red; exit 1 }

# Always run in the folder where the script lives
Set-Location $PSScriptRoot

# 0 - Sanity check
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Die "git not installed. Run: winget install --id Git.Git -e --source winget"
}

# 1 - Init repo if needed
if (-not (Test-Path ".git")) {
    Say "no repo here - initializing"
    git init -q
    git branch -M $Branch 2>$null
    Ok "git initialized"
}

# 2 - Sync remote (Safely handles new repos without crashing)
$currentRemote = git remote get-url origin 2>$null
if ($LASTEXITCODE -eq 0) {
    if ($currentRemote -ne $RemoteUrl) {
        Warn "remote changed: $currentRemote -> $RemoteUrl"
        git remote set-url origin $RemoteUrl
    }
} else {
    git remote add origin $RemoteUrl
    Ok "remote -> $RemoteUrl"
}

# 3 - Stage changes
Say "staging changes..."
git add -A

$diff = git diff --cached --name-only
if ([string]::IsNullOrWhiteSpace($diff)) {
    Warn "nothing staged - no changes to commit"
} else {
    # 4 - Block payloads from leaking to GitHub
    if ($diff -match '\.(apk|key|pem)$') {
        Die "staged file looks like a payload/secret (.apk/.key/.pem) - unstage it"
    }
    
    # 5 - Commit
    Say "committing: $Msg"
    git commit -q -m $Msg
    Ok "committed $(git rev-parse --short HEAD)"
}

# 6 - Ensure we are on the right branch
$curBranch = git rev-parse --abbrev-ref HEAD
if ($curBranch -ne $Branch) {
    Say "renaming $curBranch -> $Branch"
    git branch -M $Branch
}

# 7 - Create GitHub repo if it doesn't exist
if (Get-Command gh -ErrorAction SilentlyContinue) {
    gh repo view "$GHUser/$GHRepoName" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Say "creating github repo $GHUser/$GHRepoName"
        gh repo create "$GHUser/$GHRepoName" --private --source=. --remote=origin
        Ok "repo created"
    }
} else {
    Warn "gh CLI not installed - create the repo manually at https://github.com/new -> $GHRepoName"
}

# 8 - Push
git ls-remote --exit-code origin 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Die "remote unreachable. create the repo first: https://github.com/new -> $GHRepoName"
}

$upstream = git rev-parse --abbrev-ref --symbolic-full-name "@{u}" 2>$null
if ($LASTEXITCODE -eq 0) {
    Say "pushing to origin/$Branch"
    git push -q origin $Branch
} else {
    Say "pushing and setting upstream"
    git push -q -u origin $Branch
}

Ok "done -> https://github.com/$GHUser/$GHRepoName"