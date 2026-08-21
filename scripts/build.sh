#!/bin/bash
# Build the wiggle-monitor helper binary.
# Run from the plugin root directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/wiggle-monitor.c"
OUT="$SCRIPT_DIR/wiggle-monitor"

if ! command -v gcc &>/dev/null; then
  echo "ERROR: gcc not found. Install base-devel." >&2
  exit 1
fi

if ! pkg-config --exists libevdev 2>/dev/null; then
  echo "ERROR: libevdev not found. Install libevdev." >&2
  exit 1
fi

echo "Building wiggle-monitor..."
gcc -O2 -Wall -Wextra -Werror -pedantic -std=c11 \
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
  -Wformat -Wformat-security \
  -o "$OUT" "$SRC" \
  $(pkg-config --cflags --libs libevdev) -lm
strip --strip-unneeded "$OUT"

echo "Built: $OUT"
