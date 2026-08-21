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
  readonly property int displayDurationMs: 900
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
  // from the environment. Falls back to sensible defaults (24px, Adwaita)
  // immediately so cursor settings are always available synchronously.

  function discoverCursorSettings() {
    var hSize = Quickshell.env("HYPRCURSOR_SIZE")
    var xSize = Quickshell.env("XCURSOR_SIZE")
    var sizeStr = hSize || xSize || ""
    var size = parseInt(sizeStr)
    if (isNaN(size) || size <= 0) {
      size = 24
    }
    root.cursorSize = size

    var hTheme = Quickshell.env("HYPRCURSOR_THEME")
    var xTheme = Quickshell.env("XCURSOR_THEME")
    var theme = hTheme || xTheme || "Adwaita"
    root.cursorTheme = theme
    console.log("beacon: cursor theme=" + root.cursorTheme + " size=" + root.cursorSize)
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

  // Track how many times we've extended to cap re-shake extensions
  property int shakeExtensions: 0
  readonly property int maxExtensions: 3

  function onShakeDetected() {
    if (!root.cursorTheme || root.cursorSize <= 0) {
      console.warn("beacon: cursor settings unknown, ignoring shake")
      return
    }

    if (root.enlarged) {
      // Already enlarged — extend visibility but cap extensions
      if (root.shakeExtensions < root.maxExtensions) {
        root.shakeExtensions++
        restoreTimer.restart()
        console.log("beacon: shake during enlargement, extending (" + root.shakeExtensions + "/" + root.maxExtensions + ")")
      }
      return
    }

    enlargeCursor()
  }

  // ── Cursor Enlargement ──────────────────────────────────────────────────

  function enlargeCursor() {
    var newSize = Math.round(root.cursorSize * root.enlargeFactor)
    if (newSize > root.maxEnlargedSize) newSize = root.maxEnlargedSize
    if (newSize <= root.cursorSize) return  // Would be no change

    console.log("beacon: enlarging cursor " + root.cursorSize + " → " + newSize + " (theme: " + root.cursorTheme + ")")

    root.shakeExtensions = 0
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
    root.shakeExtensions = 0

    if (!root.cursorTheme || root.cursorSize <= 0) {
      console.warn("beacon: cannot restore, cursor settings unknown")
      return
    }

    console.log("beacon: restoring cursor → " + root.cursorSize + " (theme: " + root.cursorTheme + ")")

    // Wait briefly for any in-flight enlarge process to finish
    if (cursorProc.running) {
      pendingRestore = true
      return
    }

    runRestore()
  }

  property bool pendingRestore: false

  function runRestore() {
    root.pendingRestore = false
    cursorProc.command = [
      "hyprctl", "setcursor", root.cursorTheme, String(root.cursorSize)
    ]
    cursorProc.running = true
  }

  // Single shared process for cursor commands (avoids race between enlarge/restore)
  Process {
    id: cursorProc
    running: false
    onExited: function(code) {
      if (code !== 0) {
        console.warn("beacon: hyprctl setcursor failed with code " + code)
        // On failure, force state back to not-enlarged
        root.enlarged = false
      }
      // If a restore was pending while we were enlarging, run it now
      if (root.pendingRestore) {
        root.runRestore()
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
      console.log("beacon: restore timer fired")
      root.restoreCursor()
    }
  }

  // Safety timer: guarantees cursor is restored even if restoreTimer
  // is repeatedly extended by re-shakes. Fires after 2× display duration.
  Timer {
    id: safetyTimer
    interval: root.displayDurationMs * (root.maxExtensions + 2)
    repeat: false
    onTriggered: {
      console.warn("beacon: safety timer fired, forcing restore")
      root.restoreCursor()
    }
  }
}
