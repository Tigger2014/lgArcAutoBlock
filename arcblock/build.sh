#!/bin/sh
# build.sh — build arcblock.so + ezinject for the TV using plain Docker Desktop.
# Explicitly NOT the Rancher shim: uses /usr/local/bin/docker on the desktop-linux context.
set -eu
CD="$(cd "$(dirname "$0")" && pwd)"

DOCKER=/usr/local/bin/docker
if [ ! -x "$DOCKER" ]; then DOCKER=docker; fi
echo "==> $($DOCKER --version)"
echo "==> context: $($DOCKER context show)"

IMG=arcblock-builder
echo "==> building $IMG (soft-float armel toolchain)..."
"$DOCKER" build -t "$IMG" "$CD"

echo "==> extracting out/ ..."
rm -rf "$CD/out" && mkdir -p "$CD/out"
CTR="$("$DOCKER" create "$IMG")"
"$DOCKER" cp "$CTR":/w/out/arcblock.so "$CD/out/arcblock.so"
"$DOCKER" cp "$CTR":/w/out/ezinject "$CD/out/ezinject"
"$DOCKER" rm "$CTR" >/dev/null
file "$CD/out/"* 2>/dev/null || ls -lh "$CD/out"
echo "==> done: $CD/out"
