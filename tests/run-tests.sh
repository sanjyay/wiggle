#!/bin/bash
# Run Wiggle test suite.
# Execute from the plugin root directory.

set -uo pipefail

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$(dirname "$TEST_DIR")"

echo "Wiggle Test Suite"
echo "================="
echo ""

PASS=0
FAIL=0

run_test() {
  local name="$1"
  shift
  echo "── $name"
  if "$@"; then
    echo "   ✓ PASSED"
    ((PASS++))
  else
    echo "   ✗ FAILED"
    ((FAIL++))
  fi
  echo ""
}

# ── 1. Shake Detection Unit Tests ──
echo "Building shake detection tests..."
gcc -O2 -Wall -Wextra -Werror \
  -o "$TEST_DIR/test-shake-detector" \
  "$TEST_DIR/test-shake-detector.c" -lm 2>&1

run_test "Shake detection algorithm" "$TEST_DIR/test-shake-detector"

# ── 2. Monitor binary exists and starts ──
run_test "Monitor binary exists" test -x "$PLUGIN_DIR/scripts/wiggle-monitor"

# ── 3. Monitor auto-discovery ──
run_test "Monitor discovers mouse devices" bash -c "
  timeout 1 '$PLUGIN_DIR/scripts/wiggle-monitor' 2>&1 | grep -q 'ready'
"

# ── 4. Manifest validation ──
run_test "Manifest is valid JSON" python3 -c "
import json, sys
with open('$PLUGIN_DIR/manifest.json') as f:
    m = json.load(f)
assert m['schemaVersion'] == 1
assert m['id'] == 'io.github.sanjyay.wiggle'
assert 'service' in m['kinds']
assert 'service' in m['entryPoints']
print('  manifest.json: schema OK')
"

# ── 5. Entry point file exists ──
run_test "Service entry point exists" test -f "$PLUGIN_DIR/Wiggle.qml"

# ── 6. QML lint ──
if command -v qmllint &>/dev/null; then
  run_test "qmllint Wiggle.qml" qmllint -I "/usr/share/omarchy/shell" "$PLUGIN_DIR/Wiggle.qml" 2>&1 || true
else
  echo "── qmllint: SKIPPED (not installed)"
  echo ""
fi

# ── 7. Plugin validation ──
if command -v omarchy &>/dev/null; then
  run_test "omarchy plugin validate" omarchy plugin validate "$PLUGIN_DIR"
else
  echo "── omarchy plugin validate: SKIPPED (omarchy not in PATH)"
  echo ""
fi

# ── 8. Cursor discovery & capability unit tests ──
run_test "Cursor capabilities unit tests" python3 "$TEST_DIR/test-cursor-capabilities.py"

# ── 9. Live cursor capability discovery ──
run_test "Deterministic cursor discovery" bash -c "
  out=\$('$PLUGIN_DIR/scripts/wiggle-discover-cursor')
  echo \"\$out\" | grep -q 'STATUS=OK' && echo \"\$out\" | grep -q 'BACKEND=' && echo \"\$out\" | grep -q 'CURSOR_IMAGE=' && echo \"\$out\" | grep -q 'CURSOR_HOTSPOT='
"

# ── 10. Cursor-only architecture invariants ──
run_test "No compositor zoom implementation" bash -c "
  ! grep -R -n --exclude-dir=.git 'zoom[_]factor' '$PLUGIN_DIR'
"

run_test "Proxy is click-through and focusless" bash -c "
  grep -q 'mask: Region {}' '$PLUGIN_DIR/Wiggle.qml' &&
  grep -q 'WlrLayershell.keyboardFocus: WlrKeyboardFocus.None' '$PLUGIN_DIR/Wiggle.qml' &&
  grep -q 'wiggle-cursor-proxy' '$PLUGIN_DIR/Wiggle.qml'
"

# ── Summary ──
echo "================="
echo "Results: $PASS passed, $FAIL failed"

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
