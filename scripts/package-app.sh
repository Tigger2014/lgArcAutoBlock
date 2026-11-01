#!/bin/sh
# package-app.sh — bundle the fresh ARM binaries into the homebrew app and
# build the installable IPK. Run from your Mac after ./arcblock/build.sh.
# Usage: ./scripts/package-app.sh
# Output: arcblock-app/org.tigger2014.arcblock_1.0.0_all.ipk
set -eu

CD="$(cd "$(dirname "$0")/.." && pwd)"
APP="$CD/arcblock-app"
BIN="$APP/services/org.tigger2014.arcblock.service/bin"

if [ ! -f "$CD/arcblock/out/arcblock.so" ] || [ ! -f "$CD/arcblock/out/ezinject" ]; then
  echo "missing arcblock/out binaries — run ./arcblock/build.sh first" >&2
  exit 1
fi

# node syntax check of the service with the Mac toolchain (TV runs node 16)
node --check "$APP/services/org.tigger2014.arcblock.service/service.js" || exit 1

mkdir -p "$BIN"
cp "$CD/arcblock/out/arcblock.so" "$CD/arcblock/out/ezinject" "$BIN/"
chmod 755 "$BIN/ezinject"

rm -f "$APP"/*.ipk
(cd "$APP" && ares-package .)
ls -l "$APP"/*.ipk
