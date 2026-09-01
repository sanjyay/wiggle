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
//   6. 900ms deflate timer with seamless shrink back to 1x and safe native cursor restoration.
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
  readonly property int deflateTimeoutMs: 900
  readonly property int failsafeTimeoutMs: 5000
  readonly property int cursorHandoffTimeoutMs: 350
  readonly property int renderGraceMs: 50

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
  property bool awaitingProxyFrame: false
  property bool awaitingHiddenAck: false
  property bool awaitingShownAck: false
  property bool cursorRestoreDegraded: false
  property bool pendingActivation: false
  property real pendingMagnification: 1.0
  property int showRetryCount: 0

  // ── Render Warm-up State ────────────────────────────────────────────────
  property int warmupWindowCount: 0
  property int warmupWindowCompleteCount: 0
  property bool renderWarmupComplete: false

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
    restoreState()
    stopMonitor()
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

        if (status === "OK" && theme !== "" && sizeStr !== "" && imagePath !== "" && capability !== "none") {
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

        root.cursorConfigured = false
        root.cursorTheme = ""
        root.cursorSize = 24
        root.cursorCapability = "none"
        root.cursorImage = ""
        console.warn("wiggle: could not safely determine cursor configuration")
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
        if (line.startsWith("SHAKE ")) {
          var shakeFields = line.split(" ")
          if (shakeFields.length === 3) {
            var shakeX = parseInt(shakeFields[1])
            var shakeY = parseInt(shakeFields[2])
            if (!isNaN(shakeX) && !isNaN(shakeY)) {
              root.onShakeDetected(shakeX, shakeY)
            }
          }
        } else if (line.startsWith("POS ")) {
          var fields = line.split(" ")
          if (fields.length === 3) {
            root.cursorX = parseInt(fields[1])
            root.cursorY = parseInt(fields[2])
          }
        } else if (line === "HIDDEN") {
          root.onCursorHidden()
        } else if (line === "HIDE_FAILED") {
          root.abortActivation("native cursor hide failed")
        } else if (line === "SHOWN") {
          root.onCursorShown()
        } else if (line === "SHOW_FAILED") {
          root.onCursorShowFailed()
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

  function onShakeDetected(shakeX, shakeY) {
    if (!root.cursorConfigured || root.cursorImage === "" || root.cursorCapability === "none") {
      discoverCursorSettings()
      return
    }

    // The position and baseline scale must be committed before proxy activation.
    root.cursorX = shakeX
    root.cursorY = shakeY
    root.lastShakeTimestamp = Date.now()

    // If native-cursor restoration was not confirmed, prioritize making the
    // native cursor visible over starting another magnification cycle.
    if (root.awaitingShownAck || root.cursorRestoreDegraded) {
      root.awaitingShownAck = true
      root.cursorRestoreDegraded = false
      root.showRetryCount = 0
      requestCursorInvisible(false, "restore native cursor before reactivation")
      handoffTimer.restart()
      return
    }

    var newTarget
    if (root.targetMagnification <= 1.0) {
      newTarget = root.initialMagnification
      if (!root.renderWarmupComplete) {
        root.pendingActivation = true
        root.pendingMagnification = newTarget
        warmupWaitTimer.restart()
        return
      }
      beginActivation(newTarget)
    } else {
      newTarget = root.targetMagnification + root.overMagnification
      root.targetMagnification = newTarget
      if (root.awaitingProxyFrame || root.awaitingHiddenAck) {
        return
      }
      startScaleAnimation(newTarget)
    }

    console.log("wiggle: shake detected -> target magnification=" + newTarget +
                "x (current=" + root.currentMagnification.toFixed(2) + "x)")

    safetyTimer.restart()
  }

  function beginActivation(newTarget) {
    warmupWaitTimer.stop()
    root.pendingActivation = false
    root.pendingMagnification = 1.0
    root.currentMagnification = 1.0
    root.targetMagnification = newTarget
    root.proxyActive = true
    root.cursorRestoreDegraded = false
    root.awaitingProxyFrame = true
    // QsWindow deliberately does not expose QQuickWindow::frameSwapped.
    // Changing proxyActive schedules the proxy render; allow several refresh
    // intervals before asking the compositor to hide the native cursor.
    proxyPresentationTimer.restart()
    handoffTimer.restart()
  }

  function onProxyFramePresented() {
    if (!root.awaitingProxyFrame || !root.proxyActive) return
    root.awaitingProxyFrame = false
    root.awaitingHiddenAck = true
    requestCursorInvisible(true, "1x cursor proxy frame presented")
  }

  function startScaleAnimation(newTarget) {
    scaleAnimation.stop()
    scaleAnimation.from = root.currentMagnification
    scaleAnimation.to = newTarget
    scaleAnimation.start()
    deflateTimer.restart()
  }

  function onCursorHidden() {
    if (!root.awaitingHiddenAck || !root.proxyActive) return
    handoffTimer.stop()
    root.awaitingHiddenAck = false
    startScaleAnimation(root.targetMagnification)
  }

  function abortActivation(reason) {
    if (!root.awaitingProxyFrame && !root.awaitingHiddenAck) return
    console.warn("wiggle: " + reason + "; restoring native cursor")
    handoffTimer.stop()
    root.awaitingProxyFrame = false
    root.awaitingHiddenAck = false
    root.awaitingShownAck = true
    root.showRetryCount = 0
    requestCursorInvisible(false, "activation failure recovery")
    root.currentMagnification = 1.0
    root.targetMagnification = 1.0
    root.pendingActivation = false
    root.pendingMagnification = 1.0
    warmupWaitTimer.stop()
    deflateTimer.stop()
    safetyTimer.stop()
    handoffTimer.restart()
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
    root.awaitingShownAck = true
    root.showRetryCount = 0
    requestCursorInvisible(false, "restore native cursor after deflation")
    handoffTimer.restart()
  }

  function onCursorShown() {
    if (!root.awaitingShownAck) return
    handoffTimer.stop()
    root.awaitingShownAck = false
    root.cursorRestoreDegraded = false
    root.proxyActive = false
    root.currentMagnification = 1.0
    root.targetMagnification = 1.0
    deflateTimer.stop()
  }

  function onCursorShowFailed() {
    if (!root.awaitingShownAck) return
    if (root.showRetryCount < 1) {
      root.showRetryCount++
      console.warn("wiggle: native cursor show was not acknowledged; retrying once")
      requestCursorInvisible(false, "retry native cursor restore")
      handoffTimer.restart()
    } else {
      console.warn("wiggle: native cursor show was not confirmed; retaining the 1x proxy")
      handoffTimer.stop()
      root.awaitingShownAck = false
      root.cursorRestoreDegraded = true
      root.proxyActive = true
      root.currentMagnification = 1.0
      root.targetMagnification = 1.0
    }
  }

  function restoreState() {
    deflateTimer.stop()
    safetyTimer.stop()
    handoffTimer.stop()
    proxyPresentationTimer.stop()
    scaleAnimation.stop()
    requestCursorInvisible(false, "force restore native cursor")
    root.awaitingProxyFrame = false
    root.awaitingHiddenAck = false
    root.awaitingShownAck = false
    root.cursorRestoreDegraded = false
    root.pendingActivation = false
    root.pendingMagnification = 1.0
    root.showRetryCount = 0
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

  Timer {
    id: proxyPresentationTimer
    interval: root.renderGraceMs
    repeat: false
    onTriggered: root.onProxyFramePresented()
  }

  // ── Compositor Cursor Visibility ────────────────────────────────────────

  function requestCursorInvisible(hidden, reason) {
    if (monitorProc.running) {
      monitorProc.write(hidden ? "HIDE\n" : "SHOW\n")
    }
  }

  function registerWarmupWindow() {
    root.warmupWindowCount++
    root.renderWarmupComplete = false
  }

  function completeWarmupWindow() {
    root.warmupWindowCompleteCount++
    updateWarmupCompletion()
  }

  function unregisterWarmupWindow(wasComplete) {
    root.warmupWindowCount = Math.max(0, root.warmupWindowCount - 1)
    if (wasComplete) {
      root.warmupWindowCompleteCount = Math.max(0, root.warmupWindowCompleteCount - 1)
    }
    updateWarmupCompletion()
  }

  function updateWarmupCompletion() {
    root.renderWarmupComplete = root.warmupWindowCount > 0 &&
                                root.warmupWindowCompleteCount === root.warmupWindowCount
    if (root.renderWarmupComplete && root.pendingActivation) {
      warmupWaitTimer.stop()
      root.beginActivation(root.pendingMagnification)
      root.safetyTimer.restart()
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

      property bool surfaceArmed: backingWindowVisible
      property bool warmupActive: false
      property bool warmupComplete: false
      property bool warmupAnimationFinished: false
      property real warmupMagnification: 1.0
      property real warmupOffset: 0.0

      Component.onCompleted: {
        root.registerWarmupWindow()
        maybeStartWarmup()
      }

      Component.onDestruction: root.unregisterWarmupWindow(warmupComplete)

      onBackingWindowVisibleChanged: maybeStartWarmup()

      function maybeStartWarmup() {
        if (warmupComplete || warmupActive || !surfaceArmed ||
            cursorImageItem.status !== Image.Ready || root.cursorImage === "") {
          return
        }
        warmupMagnification = 1.0
        warmupOffset = 0.0
        warmupAnimationFinished = false
        warmupActive = true
        renderWarmupAnimation.start()
      }

      function finishWarmup() {
        if (!warmupActive) return
        warmupActive = false
        warmupComplete = true
        warmupMagnification = 1.0
        warmupOffset = 0.0
        root.completeWarmupWindow()
      }

      ParallelAnimation {
        id: renderWarmupAnimation

        NumberAnimation {
          target: proxyWindow
          property: "warmupMagnification"
          from: 1.0
          to: root.initialMagnification
          duration: root.animationDurationMs
          easing.type: Easing.InOutCubic
        }

        NumberAnimation {
          target: proxyWindow
          property: "warmupOffset"
          from: 0.0
          to: 32.0
          duration: root.animationDurationMs
          easing.type: Easing.InOutCubic
        }

        onFinished: {
          proxyWindow.warmupAnimationFinished = true
          // Toggling the supported QsWindow property forces a redraw in
          // Quickshell 0.3.1. The grace timer then lets that redraw reach the
          // compositor before this surface is declared warm.
          proxyWindow.updatesEnabled = false
          proxyWindow.updatesEnabled = true
          warmupPresentationTimer.restart()
        }
      }

      Timer {
        id: warmupPresentationTimer
        interval: root.renderGraceMs
        repeat: false
        onTriggered: {
          if (proxyWindow.warmupActive && proxyWindow.warmupAnimationFinished) {
            proxyWindow.finishWarmup()
          }
        }
      }

      Item {
        id: cursorProxyItem
        visible: proxyWindow.warmupActive ||
                 (root.proxyActive &&
                  root.cursorX >= proxyWindow.screen.x &&
                  root.cursorX < proxyWindow.screen.x + proxyWindow.screen.width &&
                  root.cursorY >= proxyWindow.screen.y &&
                  root.cursorY < proxyWindow.screen.y + proxyWindow.screen.height)
        opacity: proxyWindow.warmupActive ? 0.002 : 1.0

        // Continuous rendered dimensions
        readonly property real renderedWidth: root.cursorSize *
          (proxyWindow.warmupActive ? proxyWindow.warmupMagnification : root.currentMagnification)
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
        x: proxyWindow.warmupActive
          ? proxyWindow.warmupOffset : root.cursorX - proxyWindow.screen.x - hotX
        y: proxyWindow.warmupActive
          ? proxyWindow.warmupOffset : root.cursorY - proxyWindow.screen.y - hotY
        width: renderedWidth
        height: renderedHeight

        Image {
          id: cursorImageItem
          anchors.fill: parent
          source: root.cursorImage ? "file://" + root.cursorImage : ""
          smooth: true
          mipmap: false
          onStatusChanged: proxyWindow.maybeStartWarmup()
        }
      }
    }
  }

  // ── Deflate Timer (900ms with reset on new shake) ────────────────────────
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
        if (root.awaitingProxyFrame || root.awaitingHiddenAck) {
          root.abortActivation("activation safety timeout")
        } else if (!root.awaitingShownAck && !root.cursorRestoreDegraded) {
          scaleAnimation.stop()
          root.currentMagnification = 1.0
          root.targetMagnification = 1.0
          root.finishDeactivation()
        }
      }
    }
  }

  Timer {
    id: handoffTimer
    interval: root.cursorHandoffTimeoutMs
    repeat: false
    onTriggered: {
      if (root.awaitingProxyFrame) {
        root.abortActivation("1x cursor proxy frame was not presented")
      } else if (root.awaitingHiddenAck) {
        root.abortActivation("native cursor hide acknowledgement timed out")
      } else if (root.awaitingShownAck) {
        root.onCursorShowFailed()
      }
    }
  }

  Timer {
    id: warmupWaitTimer
    interval: 500
    repeat: false
    onTriggered: {
      if (root.pendingActivation) {
        console.warn("wiggle: discarded an early shake because render warm-up did not complete")
        root.pendingActivation = false
        root.pendingMagnification = 1.0
      }
    }
  }

}
