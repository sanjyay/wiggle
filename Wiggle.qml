// Wiggle.qml — Clean-room KDE-style Shake Cursor locator plugin for Omarchy Quickshell.
//
// "Shake your mouse to find your cursor."
//
// Architecture mirroring KDE Plasma / KWin Shake Cursor:
//   1. Clean single proxy renderer active across all surfaces (apps, wallpaper, panels, empty desktop).
//   2. Continuous magnification factor: 1x -> 3x on first shake, +1x for each subsequent shake while active.
//   3. High-resolution cursor theme asset extraction for sharp scaling.
//   4. Hotspot-anchored scaling around the exact pointer hotspot (zero tip displacement / wobble).
//   5. 200ms InOutCubic smooth re-basing animation between current and target magnification.
//   6. 2000ms deflate timer with seamless shrink back to 1x and safe native cursor restoration.
//
// MIT License

import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

Item {
  id: root

  // ── Plugin host properties (injected by Omarchy shell) ──────────────────
  property var shell: null
  property var manifest: null

  // ── KDE Behavioral Constants ─────────────────────────────────────────────
  readonly property real initialMagnification: 3.0
  readonly property real overMagnification: 1.0
  readonly property int animationDurationMs: 200
  readonly property int deflateTimeoutMs: 2000
  readonly property int failsafeTimeoutMs: 5000

  // ── Universal Theme & Asset State ───────────────────────────────────────
  property string cursorTheme: ""
  property int cursorSize: 24
  property string cursorBackend: "unknown"
  property string cursorCapability: "none"
  property string cursorImage: ""
  property int cursorHotspotX: 0
  property int cursorHotspotY: 0
  property int cursorImageWidth: 0
  property int cursorImageHeight: 0
  property bool cursorConfigured: false

  // ── Continuous Magnification & Animation State ──────────────────────────
  property real currentMagnification: 1.0
  property real targetMagnification: 1.0
  property bool proxyActive: false
  property int cursorX: -1000
  property int cursorY: -1000
  property double lastShakeTimestamp: 0
  property bool monitorStarted: false

  // ── Resolve paths ───────────────────────────────────────────────────────
  readonly property string pluginDir: {
    var url = Qt.resolvedUrl(".")
    return url.toString().replace(/^file:\/\//, "").replace(/\/$/, "")
  }
  readonly property string monitorPath: pluginDir + "/scripts/wiggle-monitor"
  readonly property string discoverPath: pluginDir + "/scripts/wiggle-discover-cursor"

  // ── Lifecycle ───────────────────────────────────────────────────────────

  Component.onCompleted: {
    discoverCursorSettings()
    startMonitor()
  }

  Component.onDestruction: {
    stopMonitor()
    restoreState()
  }

  // ── Cursor Discovery ────────────────────────────────────────────────────

  function discoverCursorSettings() {
    if (!discoverProc.running) {
      discoverProc.running = true
    }
  }

  Process {
    id: discoverProc
    running: false
    command: [root.discoverPath]

    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        var lines = text.trim().split("\n")
        var status = ""
        var theme = ""
        var themeSrc = ""
        var sizeStr = ""
        var sizeSrc = ""
        var backend = "unknown"
        var capability = "none"
        var imagePath = ""
        var hotspotStr = ""
        var imageSizeStr = ""

        for (var i = 0; i < lines.length; i++) {
          var line = lines[i].trim()
          if (line.startsWith("STATUS=")) status = line.substring(7)
          else if (line.startsWith("THEME=")) theme = line.substring(6)
          else if (line.startsWith("THEME_SRC=")) themeSrc = line.substring(10)
          else if (line.startsWith("SIZE=")) sizeStr = line.substring(5)
          else if (line.startsWith("SIZE_SRC=")) sizeSrc = line.substring(9)
          else if (line.startsWith("BACKEND=")) backend = line.substring(8)
          else if (line.startsWith("CAPABILITY=")) capability = line.substring(11)
          else if (line.startsWith("CURSOR_IMAGE=")) imagePath = line.substring(13)
          else if (line.startsWith("CURSOR_HOTSPOT=")) hotspotStr = line.substring(15)
          else if (line.startsWith("CURSOR_IMAGE_SIZE=")) imageSizeStr = line.substring(18)
        }

        if (status === "OK" && theme !== "" && sizeStr !== "") {
          var parsedSize = parseInt(sizeStr)
          if (!isNaN(parsedSize) && parsedSize > 0) {
            root.cursorTheme = theme
            root.cursorSize = parsedSize
            root.cursorBackend = backend
            root.cursorCapability = capability

            var hotspot = hotspotStr.split(",")
            var imageSize = imageSizeStr.split(",")
            root.cursorImage = imagePath
            root.cursorHotspotX = parseInt(hotspot[0]) || 0
            root.cursorHotspotY = parseInt(hotspot[1]) || 0
            root.cursorImageWidth = parseInt(imageSize[0]) || 0
            root.cursorImageHeight = parseInt(imageSize[1]) || 0
            root.cursorConfigured = true

            console.log("wiggle: discovered cursor theme=" + root.cursorTheme +
                        " (" + themeSrc + "), size=" + root.cursorSize +
                        " (" + sizeSrc + "), backend=" + backend +
                        ", image=" + imagePath + " (" + root.cursorImageWidth + "x" + root.cursorImageHeight +
                        ", hotspot=" + root.cursorHotspotX + "," + root.cursorHotspotY + ")")
            return
          }
        }

        if (!root.cursorConfigured) {
          root.cursorTheme = ""
          root.cursorSize = 24
          console.warn("wiggle: could not safely determine cursor configuration")
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
        } else if (line.startsWith("POS ")) {
          var fields = line.split(" ")
          if (fields.length === 3) {
            root.cursorX = parseInt(fields[1])
            root.cursorY = parseInt(fields[2])
          }
        }
      }
    }

    stderr: SplitParser {
      splitMarker: "\n"
      onRead: function(data) {
        var line = data.trim()
        if (line) console.log("wiggle-monitor: " + line)
      }
    }

    onExited: function(code) {
      root.monitorStarted = false
      if (code !== 0) {
        console.warn("wiggle: monitor exited with code " + code)
      }
      if (root.proxyActive || root.targetMagnification > 1.0) {
        root.restoreState()
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

  // ── KDE-Style Shake Handling & Magnification ─────────────────────────────

  function onShakeDetected() {
    if (!root.cursorConfigured || root.cursorImage === "" || root.cursorCapability === "none") {
      return
    }

    root.lastShakeTimestamp = Date.now()

    var newTarget
    if (root.targetMagnification <= 1.0) {
      newTarget = root.initialMagnification
      activateProxy()
    } else {
      newTarget = root.targetMagnification + root.overMagnification
    }

    root.targetMagnification = newTarget

    // Rebase animation smoothly from current rendered magnification to new target
    scaleAnimation.stop()
    scaleAnimation.from = root.currentMagnification
    scaleAnimation.to = newTarget
    scaleAnimation.start()

    console.log("wiggle: shake detected -> target magnification=" + newTarget +
                "x (current=" + root.currentMagnification.toFixed(2) + "x)")

    deflateTimer.restart()
    safetyTimer.restart()
  }

  function activateProxy() {
    root.proxyActive = true
    root.currentMagnification = 1.0
    requestCursorInvisible(true, "cursor proxy activated")
  }

  function deflate() {
    console.log("wiggle: deflate timeout reached, returning to 1.0x")
    root.targetMagnification = 1.0
    scaleAnimation.stop()
    scaleAnimation.from = root.currentMagnification
    scaleAnimation.to = 1.0
    scaleAnimation.start()
  }

  function finishDeactivation() {
    console.log("wiggle: reached 1.0x baseline, restoring native cursor")
    requestCursorInvisible(false, "restore native cursor after deflation")
    root.proxyActive = false
    root.currentMagnification = 1.0
    root.targetMagnification = 1.0
    deflateTimer.stop()
  }

  function restoreState() {
    deflateTimer.stop()
    safetyTimer.stop()
    scaleAnimation.stop()
    requestCursorInvisible(false, "force restore native cursor")
    root.proxyActive = false
    root.currentMagnification = 1.0
    root.targetMagnification = 1.0
  }

  // ── Magnification Animation (200ms InOutCubic) ───────────────────────────

  NumberAnimation {
    id: scaleAnimation
    target: root
    property: "currentMagnification"
    duration: root.animationDurationMs
    easing.type: Easing.InOutCubic

    onRunningChanged: {
      if (!running) {
        if (Math.abs(root.currentMagnification - 1.0) < 0.05 && root.targetMagnification <= 1.0) {
          root.finishDeactivation()
        }
      }
    }
  }

  // ── Compositor Cursor Visibility ────────────────────────────────────────

  function requestCursorInvisible(hidden, reason) {
    if (monitorProc.running) {
      monitorProc.write(hidden ? "HIDE\n" : "SHOW\n")
    }
  }

  // ── Overlay Proxy Renderer ───────────────────────────────────────────────

  Variants {
    model: Quickshell.screens

    PanelWindow {
      id: proxyWindow
      required property var modelData
      screen: modelData
      visible: true
      color: "transparent"
      exclusionMode: ExclusionMode.Ignore
      WlrLayershell.namespace: "wiggle-cursor-proxy"
      WlrLayershell.layer: WlrLayer.Overlay
      WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
      anchors { top: true; bottom: true; left: true; right: true }
      mask: Region {}

      Item {
        id: cursorProxyItem
        visible: root.proxyActive &&
                 root.cursorX >= proxyWindow.screen.x &&
                 root.cursorX < proxyWindow.screen.x + proxyWindow.screen.width &&
                 root.cursorY >= proxyWindow.screen.y &&
                 root.cursorY < proxyWindow.screen.y + proxyWindow.screen.height

        // Continuous rendered dimensions
        readonly property real renderedWidth: root.cursorSize * root.currentMagnification
        readonly property real renderedHeight: root.cursorImageWidth > 0
          ? (renderedWidth * (root.cursorImageHeight / root.cursorImageWidth))
          : renderedWidth

        // Scale ratio relative to source image asset
        readonly property real assetScale: root.cursorImageWidth > 0
          ? renderedWidth / root.cursorImageWidth
          : 1.0

        // Scaled hotspot in rendered coordinate space
        readonly property real hotX: root.cursorHotspotX * assetScale
        readonly property real hotY: root.cursorHotspotY * assetScale

        // Position top-left so that (hotX, hotY) lands exactly on (cursorX, cursorY)
        x: root.cursorX - proxyWindow.screen.x - hotX
        y: root.cursorY - proxyWindow.screen.y - hotY
        width: renderedWidth
        height: renderedHeight

        Image {
          anchors.fill: parent
          source: root.cursorImage ? "file://" + root.cursorImage : ""
          smooth: true
          mipmap: true
        }
      }
    }
  }

  // ── Deflate Timer (2000ms with reset on new shake) ───────────────────────
  Timer {
    id: deflateTimer
    interval: root.deflateTimeoutMs
    repeat: false
    onTriggered: root.deflate()
  }

  // ── Safety Failsafe Timer ────────────────────────────────────────────────
  Timer {
    id: safetyTimer
    interval: root.failsafeTimeoutMs
    repeat: false
    onTriggered: {
      var elapsed = Date.now() - root.lastShakeTimestamp
      if (elapsed < root.deflateTimeoutMs + root.animationDurationMs + 500) {
        safetyTimer.restart()
        return
      }
      if (root.proxyActive || root.targetMagnification > 1.0) {
        console.warn("wiggle: safety timer expired (" + Math.round(elapsed) + "ms since last shake), restoring baseline")
        root.restoreState()
      }
    }
  }
}
