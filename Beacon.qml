// Beacon.qml — Service entry point for the Beacon plugin.
//
// "Shake your mouse to find your cursor."
//
// Launches the bundled beacon-monitor helper which detects mouse shake
// gestures via evdev and outputs "SHAKE" on stdout. On receiving SHAKE,
// temporarily enlarges the cursor using hyprctl setcursor, then restores
// the exact original theme and size after movement settles.
//
// Safety & Integrity Guarantees:
//   - Discovers exact cursor theme and size on load via beacon-discover-cursor.
//   - If actual cursor configuration cannot be determined safely, remains INACTIVE.
//   - Never falls back to guessed or default values.
//   - Restoration always returns to exact captured original theme and size.
//   - Clean shutdown (plugin disable / shell restart) restores original cursor.
//   - Process errors or crashes restore original cursor.

import QtQuick
import Quickshell
import Quickshell.Io

Item {
  id: root

  // ── Plugin host properties (injected by Omarchy shell) ──────────────────
  property var shell: null
  property var manifest: null

  // ── Configuration ───────────────────────────────────────────────────────
  // Multiplier for cursor enlargement (3× original size)
  readonly property real enlargeFactor: 3.0
  // Minimum enlarged size to guarantee unmistakable visibility
  readonly property int minEnlargedSize: 72
  // Maximum enlarged cursor size in pixels
  readonly property int maxEnlargedSize: 128
  // How long the cursor stays enlarged after the last shake (ms)
  readonly property int displayDurationMs: 900

  // ── Internal State ──────────────────────────────────────────────────────
  property string cursorTheme: ""
  property int cursorSize: 0
  property bool cursorConfigured: false
  property bool enlarged: false
  property bool monitorStarted: false
  property bool pendingRestore: false

  // ── Resolve paths ───────────────────────────────────────────────────────
  readonly property string pluginDir: {
    var url = Qt.resolvedUrl(".")
    return url.toString().replace(/^file:\/\//, "").replace(/\/$/, "")
  }
  readonly property string monitorPath: pluginDir + "/scripts/beacon-monitor"
  readonly property string discoverPath: pluginDir + "/scripts/beacon-discover-cursor"

  // ── Lifecycle ───────────────────────────────────────────────────────────

  Component.onCompleted: {
    discoverCursorSettings()
    startMonitor()
  }

  Component.onDestruction: {
    stopMonitor()
    restoreCursor()
  }

  // ── Cursor Discovery ────────────────────────────────────────────────────
  // Runs the deterministic discovery helper. If discovery fails or is
  // ambiguous, cursorConfigured remains false and Beacon remains inactive.

  property string _discoveredStatus: ""
  property string _discoveredTheme: ""
  property string _discoveredThemeSrc: ""
  property string _discoveredSize: ""
  property string _discoveredSizeSrc: ""

  function discoverCursorSettings() {
    root._discoveredStatus = ""
    root._discoveredTheme = ""
    root._discoveredThemeSrc = ""
    root._discoveredSize = ""
    root._discoveredSizeSrc = ""
    discoverProc.running = true
  }

  Process {
    id: discoverProc
    running: false
    command: [root.discoverPath]

    stdout: SplitParser {
      splitMarker: "\n"
      onRead: function(data) {
        var line = data.trim()
        if (!line) return
        if (line.startsWith("STATUS=")) {
          root._discoveredStatus = line.substring(7)
        } else if (line.startsWith("THEME=")) {
          root._discoveredTheme = line.substring(6)
        } else if (line.startsWith("THEME_SRC=")) {
          root._discoveredThemeSrc = line.substring(10)
        } else if (line.startsWith("SIZE=")) {
          root._discoveredSize = line.substring(5)
        } else if (line.startsWith("SIZE_SRC=")) {
          root._discoveredSizeSrc = line.substring(9)
        }
      }
    }

    onExited: function(code) {
      if (code === 0 && root._discoveredStatus === "OK" &&
          root._discoveredTheme !== "" && root._discoveredSize !== "") {
        var parsedSize = parseInt(root._discoveredSize)
        if (!isNaN(parsedSize) && parsedSize > 0) {
          root.cursorTheme = root._discoveredTheme
          root.cursorSize = parsedSize
          root.cursorConfigured = true
          console.log("beacon: discovered cursor theme=" + root.cursorTheme +
                      " (" + root._discoveredThemeSrc + "), size=" + root.cursorSize +
                      " (" + root._discoveredSizeSrc + ")")
          return
        }
      }

      root.cursorConfigured = false
      root.cursorTheme = ""
      root.cursorSize = 0
      console.warn("beacon: could not safely determine cursor configuration. Beacon will remain inactive.")
    }
  }

  // ── Monitor Process ─────────────────────────────────────────────────────

  Process {
    id: monitorProc
    running: false
    command: [root.monitorPath]

    stdout: SplitParser {
      splitMarker: "\n"
      onRead: function(data) {
        var line = data.trim()
        if (line === "SHAKE") {
          root.onShakeDetected()
        }
      }
    }

    stderr: SplitParser {
      splitMarker: "\n"
      onRead: function(data) {
        var line = data.trim()
        if (line) console.log("beacon-monitor: " + line)
      }
    }

    onExited: function(code) {
      root.monitorStarted = false
      if (code !== 0) {
        console.warn("beacon: monitor exited with code " + code)
      }
      // Ensure cursor is restored if monitor dies unexpectedly
      if (root.enlarged) {
        root.restoreCursor()
      }
    }
  }

  function startMonitor() {
    if (monitorProc.running) return
    monitorProc.running = true
    root.monitorStarted = true
  }

  function stopMonitor() {
    if (monitorProc.running) {
      monitorProc.running = false
    }
    root.monitorStarted = false
  }

  // ── Shake Handler ───────────────────────────────────────────────────────

  function onShakeDetected() {
    // Safety check: do not enlarge if cursor configuration is not verified
    if (!root.cursorConfigured || !root.cursorTheme || root.cursorSize <= 0) {
      console.warn("beacon: cursor configuration unverified, ignoring shake")
      return
    }

    if (root.enlarged) {
      // Re-shake while already enlarged: extend duration
      restoreTimer.restart()
      return
    }

    enlargeCursor()
  }

  // ── Cursor Enlargement ──────────────────────────────────────────────────

  function enlargeCursor() {
    var target = Math.round(root.cursorSize * root.enlargeFactor)
    var newSize = Math.max(target, root.minEnlargedSize)
    if (newSize > root.maxEnlargedSize) newSize = root.maxEnlargedSize
    if (newSize <= root.cursorSize) return

    console.log("beacon: enlarging cursor " + root.cursorSize + " -> " + newSize +
                " (theme: " + root.cursorTheme + ")")

    root.enlarged = true
    cursorProc.command = [
      "hyprctl", "setcursor", root.cursorTheme, String(newSize)
    ]
    cursorProc.running = true
    restoreTimer.restart()
    safetyTimer.restart()
  }

  function restoreCursor() {
    restoreTimer.stop()
    safetyTimer.stop()

    if (!root.enlarged) return
    root.enlarged = false

    if (!root.cursorConfigured || !root.cursorTheme || root.cursorSize <= 0) {
      console.warn("beacon: cannot restore, original cursor state unknown")
      return
    }

    console.log("beacon: restoring exact original cursor -> " + root.cursorSize +
                " (theme: " + root.cursorTheme + ")")

    if (cursorProc.running) {
      root.pendingRestore = true
      return
    }

    runRestore()
  }

  function runRestore() {
    root.pendingRestore = false
    cursorProc.command = [
      "hyprctl", "setcursor", root.cursorTheme, String(root.cursorSize)
    ]
    cursorProc.running = true
  }

  // Single shared process for hyprctl setcursor invocations
  Process {
    id: cursorProc
    running: false
    onExited: function(code) {
      if (code !== 0) {
        console.warn("beacon: hyprctl setcursor failed with code " + code)
        root.enlarged = false
      }
      if (root.pendingRestore) {
        root.runRestore()
      }
    }
  }

  // ── Restore Timer ───────────────────────────────────────────────────────
  Timer {
    id: restoreTimer
    interval: root.displayDurationMs
    repeat: false
    onTriggered: {
      root.restoreCursor()
    }
  }

  // Safety timer: guarantees exact restoration after 3 seconds max
  Timer {
    id: safetyTimer
    interval: 3000
    repeat: false
    onTriggered: {
      console.warn("beacon: safety timer fired, enforcing restore")
      root.restoreCursor()
    }
  }
}
