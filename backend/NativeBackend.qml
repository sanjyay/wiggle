import QtQuick
import Quickshell
import Quickshell.Io

Item {
  id: root

  property var shell: null
  property var manifest: null
  property var settings: ({})

  readonly property bool effectEnabled: settingBool("enabled", true)
  readonly property real sensitivity: settingReal("sensitivity", 4.0, 1.0, 10.0)
  readonly property real maximumScale: settingReal("maxScale", 4.0, 1.0, 8.0)
  readonly property string runtimeConfigPath: Quickshell.env("XDG_RUNTIME_DIR") + "/wiggle-native.conf"
  readonly property alias backendStatus: status

  function setting(name, fallback) {
    var value = settings ? settings[name] : undefined
    return value === undefined || value === null ? fallback : value
  }

  function settingBool(name, fallback) {
    var value = setting(name, fallback)
    return value === true || value === 1 || String(value).toLowerCase() === "true"
  }

  function settingReal(name, fallback, minimum, maximum) {
    var value = Number(setting(name, fallback))
    if (!isFinite(value)) value = fallback
    return Math.max(minimum, Math.min(maximum, value))
  }

  function checkBackend() {
    if (!detectProcess.running) detectProcess.running = true
  }

  function scheduleConfiguration() {
    if (status.available) configureTimer.restart()
  }

  function applyConfiguration() {
    if (!status.available || root.runtimeConfigPath === "/wiggle-native.conf") return
    runtimeConfigFile.setText(
      "1 " + (root.effectEnabled ? "1" : "0") + " "
        + root.sensitivity.toFixed(3) + " "
        + root.maximumScale.toFixed(3) + "\n")
  }

  onEffectEnabledChanged: scheduleConfiguration()
  onSensitivityChanged: scheduleConfiguration()
  onMaximumScaleChanged: scheduleConfiguration()
  Component.onCompleted: checkBackend()

  BackendStatus { id: status }

  FileView {
    id: runtimeConfigFile
    path: root.runtimeConfigPath
    watchChanges: false
    atomicWrites: true
    printErrors: false
  }

  Timer {
    id: configureTimer
    interval: 50
    repeat: false
    onTriggered: root.applyConfiguration()
  }

  Process {
    id: detectProcess
    running: false
    command: ["hyprctl", "-j", "plugin", "list"]

    stdout: StdioCollector {
      id: detectOutput
      waitForEnd: true
    }

    stderr: StdioCollector {
      id: detectError
      waitForEnd: true
    }

    onExited: function(exitCode) {
      if (exitCode !== 0) {
        status.state = "error"
        status.error = detectError.text.trim() || "Unable to query Hyprland plugin status"
        console.warn("wiggle: " + status.error)
        return
      }

      try {
        var plugins = JSON.parse(detectOutput.text || "[]")
        for (var index = 0; index < plugins.length; ++index) {
          if (plugins[index].name === "wiggle-native") {
            status.state = "available"
            status.version = String(plugins[index].version || "unknown")
            status.error = ""
            root.scheduleConfiguration()
            console.log("wiggle: native backend available, version=" + status.version)
            return
          }
        }
      } catch (error) {
        status.state = "error"
        status.error = "Invalid response from hyprctl: " + error
        console.warn("wiggle: " + status.error)
        return
      }

      if (!hyprpmProcess.running) hyprpmProcess.running = true
    }
  }

  Process {
    id: hyprpmProcess
    running: false
    command: ["hyprpm", "list"]

    stdout: StdioCollector {
      id: hyprpmOutput
      waitForEnd: true
    }

    stderr: StdioCollector {
      id: hyprpmError
      waitForEnd: true
    }

    onExited: function(exitCode) {
      var combined = (hyprpmOutput.text + "\n" + hyprpmError.text).toLowerCase()
      if (combined.indexOf("wiggle-native") !== -1) {
        status.state = "error"
        status.error = "The native backend is installed but not loaded; run hyprpm reload and check Hyprland compatibility"
      } else {
        status.state = "unavailable"
        status.error = exitCode === 0
          ? "Native backend missing; install this repository with hyprpm and enable wiggle-native"
          : "Native backend is not loaded and hyprpm status could not be read"
      }
      console.warn("wiggle: " + status.error)
    }
  }

}
