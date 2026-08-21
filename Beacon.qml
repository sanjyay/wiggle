// Beacon.qml — Service entry point for the Beacon plugin.
//
// "Shake your mouse to find your cursor."
//
// Launches the bundled beacon-monitor helper which detects mouse shake
// gestures via evdev and outputs "SHAKE" on stdout. On receiving SHAKE,
// temporarily enlarges the cursor using hyprctl setcursor, then restores
// the original size after a timeout.
//
// Lifecycle:
//   - Started when the plugin is loaded (keepLoaded: true)
//   - Stops the helper when the plugin is unloaded
//   - Only one helper instance runs at a time
//   - Original cursor theme/size are always restored on shutdown

import QtQuick
import Quickshell
import Quickshell.Io

Item {
  id: root

  // ── Plugin host properties (injected by Omarchy shell) ──────────────────
  property var shell: null
  property var manifest: null

  // ── Configuration ───────────────────────────────────────────────────────
  // Multiplier for cursor enlargement (2.5× original size)
  readonly property real enlargeFactor: 2.5
  // Maximum enlarged cursor size in pixels
  readonly property int maxEnlargedSize: 96
  // How long the cursor stays enlarged after the last shake (ms)
  readonly property int displayDurationMs: 2000
  // Minimum interval between cursor resize commands (ms)
  readonly property int resizeDebounceMs: 100

  // ── Internal State ──────────────────────────────────────────────────────
  property string cursorTheme: ""
  property int cursorSize: 0
  property bool enlarged: false
  property bool monitorStarted: false

  // ── Resolve paths ───────────────────────────────────────────────────────
  readonly property string pluginDir: {
    var url = Qt.resolvedUrl(".")
    return url.toString().replace(/^file:\/\//, "").replace(/\/$/, "")
  }
  readonly property string monitorPath: pluginDir + "/scripts/beacon-monitor"

  // ── Lifecycle ───────────────────────────────────────────────────────────

  Component.onCompleted: {
    discoverCursorSettings()
    startMonitor()
  }

  Component.onDestruction: {
    stopMonitor()
    restoreCursor()
  }

  // ── Cursor Theme Discovery ──────────────────────────────────────────────
  // Reads XCURSOR_THEME, XCURSOR_SIZE, HYPRCURSOR_THEME, HYPRCURSOR_SIZE
  // from the environment. Falls back to sensible defaults only if all
  // sources are empty.

  function discoverCursorSettings() {
    // Prefer HYPRCURSOR_SIZE, fall back to XCURSOR_SIZE
    var hSize = Quickshell.env("HYPRCURSOR_SIZE")
    var xSize = Quickshell.env("XCURSOR_SIZE")
    var sizeStr = hSize || xSize || ""
    var size = parseInt(sizeStr)
    if (isNaN(size) || size <= 0) {
      console.warn("beacon: could not determine cursor size from env, defaulting to 24")
      size = 24
    }
    root.cursorSize = size

    // Theme: prefer HYPRCURSOR_THEME, then XCURSOR_THEME
    var hTheme = Quickshell.env("HYPRCURSOR_THEME")
    var xTheme = Quickshell.env("XCURSOR_THEME")
    var theme = hTheme || xTheme || ""
    if (!theme) {
      // Read from hyprctl if env vars are empty
      themeDiscoveryProc.running = true
      return
    }
    root.cursorTheme = theme
    console.log("beacon: cursor theme=" + root.cursorTheme + " size=" + root.cursorSize)
  }

  // Fallback: ask hyprctl for the current cursor theme
  Process {
    id: themeDiscoveryProc
    running: false
    command: ["hyprctl", "-j", "cursorpos"]

    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        // If theme is still empty, try reading the Hyprland config default
        // For now, use "Adwaita" as the safe fallback (it's the system default)
        if (!root.cursorTheme) {
          root.cursorTheme = "Adwaita"
          console.log("beacon: using fallback cursor theme=Adwaita size=" + root.cursorSize)
        }
      }
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
      monitorProc.running = false  // Sends SIGTERM to child
    }
    root.monitorStarted = false
  }

  // ── Shake Handler ───────────────────────────────────────────────────────

  function onShakeDetected() {
    if (!root.cursorTheme || root.cursorSize <= 0) {
      console.warn("beacon: cursor settings unknown, ignoring shake")
      return
    }

    if (root.enlarged) {
      // Already enlarged — just restart the timer (extend visibility)
      restoreTimer.restart()
      return
    }

    enlargeCursor()
  }

  // ── Cursor Enlargement ──────────────────────────────────────────────────

  function enlargeCursor() {
    var newSize = Math.round(root.cursorSize * root.enlargeFactor)
    if (newSize > root.maxEnlargedSize) newSize = root.maxEnlargedSize
    if (newSize <= root.cursorSize) return  // Would be no change

    setCursorProc.command = [
      "hyprctl", "setcursor", root.cursorTheme, String(newSize)
    ]
    setCursorProc.running = true
    root.enlarged = true
    restoreTimer.restart()
  }

  function restoreCursor() {
    if (!root.enlarged) return

    restoreTimer.stop()
    root.enlarged = false

    if (!root.cursorTheme || root.cursorSize <= 0) return

    restoreCursorProc.command = [
      "hyprctl", "setcursor", root.cursorTheme, String(root.cursorSize)
    ]
    restoreCursorProc.running = true
  }

  // Process for setting cursor (enlargement)
  Process {
    id: setCursorProc
    running: false
    onExited: function(code) {
      if (code !== 0) {
        console.warn("beacon: hyprctl setcursor (enlarge) failed with code " + code)
        root.enlarged = false
      }
    }
  }

  // Process for restoring cursor (separate to avoid conflicts)
  Process {
    id: restoreCursorProc
    running: false
    onExited: function(code) {
      if (code !== 0) {
        console.warn("beacon: hyprctl setcursor (restore) failed with code " + code)
      }
    }
  }

  // ── Restore Timer ───────────────────────────────────────────────────────
  // After the cursor has been enlarged, wait for movement to settle,
  // then restore to original size.

  Timer {
    id: restoreTimer
    interval: root.displayDurationMs
    repeat: false
    onTriggered: {
      root.restoreCursor()
    }
  }
}
