#!/bin/sh
# deploy.sh — install ARC Sound Override Blocker on the TV: app files + service + ARM
# binaries, then elevate the service and register boot persistence.
# Installs DISABLED (no config = off): flip the toggle in the app (or
# flip the toggle in the TV app) to hook eim — that restarts eim live.
# Usage: ./scripts/deploy.sh [root@<tv-ip>]
set -eu

TARGET="${1:-${TV_SSH:-root@${TV_IP:-lg-tv}}}"
CD="$(cd "$(dirname "$0")/.." && pwd)"
APP=org.tigger2014.arcblock

if [ ! -f "$CD/arcblock/out/arcblock.so" ] || [ ! -f "$CD/arcblock/out/ezinject" ]; then
  echo "missing arcblock/out/arcblock.so or arcblock/out/ezinject — run ./arcblock/build.sh first" >&2
  exit 1
fi
node --check "$CD/arcblock-app/services/$APP.service/service.js" || exit 1

echo "==> target: $TARGET"
ssh -o ConnectTimeout=10 "$TARGET" 'mkdir -p /media/developer/apps/usr/palm/applications/org.tigger2014.arcblock /media/developer/apps/usr/palm/services/org.tigger2014.arcblock.service/bin'

echo "==> pushing app + service..."
scp -O "$CD/arcblock-app/app.js" "$CD/arcblock-app/index.html" "$CD/arcblock-app/style.css" "$CD/arcblock-app/appinfo.json" \
  "$TARGET:/media/developer/apps/usr/palm/applications/org.tigger2014.arcblock/"
scp -O "$CD/arcblock-app/services/$APP.service/service.js" "$CD/arcblock-app/services/$APP.service/package.json" "$CD/arcblock-app/services/$APP.service/services.json" \
  "$TARGET:/media/developer/apps/usr/palm/services/$APP.service/"
scp -O "$CD/arcblock/out/arcblock.so" "$CD/arcblock/out/ezinject" \
  "$TARGET:/media/developer/apps/usr/palm/services/$APP.service/bin/"

echo "==> checksum check (transfer corruption guard)..."
LOCAL_SUM="$(md5sum "$CD/arcblock/out/ezinject" | cut -d' ' -f1)" 2>/dev/null || LOCAL_SUM="$(md5 -q "$CD/arcblock/out/ezinject")"
REMOTE_SUM="$(ssh -o ConnectTimeout=10 "$TARGET" 'md5sum /media/developer/apps/usr/palm/services/org.tigger2014.arcblock.service/bin/ezinject 2>/dev/null | cut -d" " -f1')"
if [ "$LOCAL_SUM" != "$REMOTE_SUM" ]; then echo "md5 MISMATCH local=$LOCAL_SUM remote=$REMOTE_SUM — aborting" >&2; exit 1; fi
echo "md5 ok: $LOCAL_SUM"

echo "==> elevate + boot persistence (idempotent)..."
ssh -o ConnectTimeout=10 "$TARGET" 'chmod 755 /media/developer/apps/usr/palm/services/org.tigger2014.arcblock.service/bin/ezinject'
ssh -o ConnectTimeout=30 "$TARGET" '/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service org.tigger2014.arcblock.service > /dev/null 2>&1; luna-send -n 1 luna://org.tigger2014.arcblock.service/autostart "{}"'
echo "==> status (expect enabled:false, unhooked — fresh installs are OFF):"
ssh -o ConnectTimeout=30 "$TARGET" 'timeout 60 luna-send -n 1 luna://org.tigger2014.arcblock.service/getStatus "{}"'
echo
echo "done. To hook eim: open ARC Sound Override Blocker on the TV and flip ON"
