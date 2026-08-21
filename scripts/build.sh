#!/bin/bash
# Build the beacon-monitor helper binary.
# Run from the plugin root directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/beacon-monitor.c"
OUT="$SCRIPT_DIR/beacon-monitor"

if ! command -v gcc &>/dev/null; then
  echo "ERROR: gcc not found. Install base-devel." >&2
  exit 1
fi

if ! pkg-config --exists libevdev 2>/dev/null; then
  echo "ERROR: libevdev not found. Install libevdev." >&2
  exit 1
fi

echo "Building beacon-monitor..."
gcc -O2 -Wall -Wextra \
  -o "$OUT" "$SRC" \
  $(pkg-config --cflags --libs libevdev) -lm

echo "Built: $OUT"
