#!/bin/sh
# check-abi.sh — TV-ABI contract: every artifact must be soft-float ARMv7-A
# (e_flags 0x5000200, like eim). Fails the build otherwise.
set -eu
for f in "$@"; do
  FLAGS="$(arm-linux-gnueabi-readelf -h "$f" | grep -a Flags)"
  echo "$f: $FLAGS"
  case "$FLAGS" in
    *0x5000200*) ;;
    *) echo "ABI CONTRACT VIOLATION: $f is not soft-float 0x5000200" >&2; exit 1 ;;
  esac
done
