#!/usr/bin/env bash
# language: bash, file: push.sh
# *GitHub-only push. No build, no flash, no arduino-cli.*
# *usage: ./push.sh "commit message"*

set -euo pipefail

# ============ CONFIG — edit these ============
REPO_NAME="esp32s3_kit"
GH_USER="LILBIG4DEV"
GH_REPO_NAME="esp32s3_kit"
BRANCH="main"
DEFAULT_MSG="update firmware $(date +%Y-%m-%d\ %H:%M)"
REMOTE_URL="https://github.com/${GH_USER}/${GH_REPO_NAME}.git"
# =============================================

say()  { printf '\033[1;36m[push]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ok\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m  !!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[died]\033[0m %s\n' "$*"; exit 1; }

MSG="${1:-$DEFAULT_MSG}"
cd "$(dirname "$0")"

# ---------------------------------------------------------------
# 0 — sanity
# ---------------------------------------------------------------
command -v git >/dev/null 2>&1 || die "git not installed"

# ---------------------------------------------------------------
# 1 — scaffold on first run (only if no .git yet)
# ---------------------------------------------------------------
if [ ! -d .git ]; then
  say "no repo here — scaffolding"

  # make sure data/ exists
  mkdir -p data
  touch data/.gitkeep

  # .gitignore
  if [ ! -f .gitignore ]; then
    cat > .gitignore <<'EOF'
build/
*.bin
*.elf
*.map
*.hex
*.d
*.o
*.a
*.su
.arduino/
.cache/
.vscode/
.idea/
*.code-workspace
.DS_Store
Thumbs.db
desktop.ini
secrets.h
*.key
*.pem
data/*.apk
data/*.bin
!data/.gitkeep
__pycache__/
*.pyc
EOF
    ok ".gitignore written"
  fi

  # partitions.csv (if missing)
  if [ ! -f partitions.csv ]; then
    cat > partitions.csv <<'EOF'
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x3C0000,
spiffs,   data, spiffs,  0x3D0000, 0x420000,
EOF
    ok "partitions.csv written"
  fi

  git init -q
  git branch -M "$BRANCH" 2>/dev/null || true
  ok "git initialized"
fi

# ---------------------------------------------------------------
# 2 — keep remote in sync with config
# ---------------------------------------------------------------
if git remote get-url origin >/dev/null 2>&1; then
  CUR="$(git remote get-url origin)"
  [ "$CUR" = "$REMOTE_URL" ] || { warn "remote changed: $CUR → $REMOTE_URL"; git remote set-url origin "$REMOTE_URL"; }
else
  git remote add origin "$REMOTE_URL"
  ok "remote → $REMOTE_URL"
fi

# ---------------------------------------------------------------
# 3 — stage
# ---------------------------------------------------------------
say "staging changes"
git add -A

if git diff --cached --quiet; then
  warn "nothing staged — no changes to commit"
else
  # refuse to push payloads / secrets even if .gitignore was bypassed
  if git diff --cached --name-only | grep -E '\.(apk|key|pem)$' >/dev/null; then
    die "staged file looks like a payload/secret (.apk/.key/.pem) — unstage it"
  fi
  say "committing: $MSG"
  git commit -q -m "$MSG"
  ok "committed $(git rev-parse --short HEAD)"
fi

# ---------------------------------------------------------------
# 4 — ensure we're on the right branch
# ---------------------------------------------------------------
git symbolic-ref -q HEAD >/dev/null || die "detached HEAD — checkout a branch first"
CUR_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[ "$CUR_BRANCH" = "$BRANCH" ] || { say "renaming $CUR_BRANCH → $BRANCH"; git branch -M "$BRANCH"; }

# ---------------------------------------------------------------
# 5 — create github repo if it doesn't exist (gh CLI)
# ---------------------------------------------------------------
if command -v gh >/dev/null 2>&1; then
  if ! gh repo view "$GH_USER/$GH_REPO_NAME" >/dev/null 2>&1; then
    say "creating github repo $GH_USER/$GH_REPO_NAME"
    gh repo create "$GH_USER/$GH_REPO_NAME" --private --source=. --remote=origin
    ok "repo created"
  fi
else
  warn "gh CLI not installed — you must create the repo manually at:"
  warn "  https://github.com/new  → name: $GH_REPO_NAME"
fi

# ---------------------------------------------------------------
# 6 — push
# ---------------------------------------------------------------
if ! git ls-remote --exit-code origin >/dev/null 2>&1; then
  die "remote unreachable. create the repo first: https://github.com/new → $GH_REPO_NAME"
fi

if git rev-parse --abbrev-ref --symbolic-full-name "@{u}" >/dev/null 2>&1; then
  say "pushing to origin/$BRANCH"
  git push -q origin "$BRANCH"
else
  say "pushing and setting upstream"
  git push -q -u origin "$BRANCH"
fi

ok "done → https://github.com/${GH_USER}/${GH_REPO_NAME}"