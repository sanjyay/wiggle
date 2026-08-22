#!/bin/bash
# Rebuild the committed helper with the pinned container toolchain.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK_DIR="$(mktemp -d)"
trap 'rm -r -- "$WORK_DIR"' EXIT

"$SCRIPT_DIR/build-wiggle-monitor" "$WORK_DIR"
install -m 0755 "$WORK_DIR/wiggle-monitor" "$SCRIPT_DIR/wiggle-monitor"
echo "Built: $SCRIPT_DIR/wiggle-monitor"
