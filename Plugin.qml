import QtQuick
import Quickshell

Item {
  id: root

  property var shell: null
  property var manifest: null

  readonly property var wiggleSettings: manifest && manifest.wiggle ? manifest.wiggle : ({})
  readonly property string configuredBackend: {
    var environmentBackend = Quickshell.env("WIGGLE_BACKEND")
    if (environmentBackend === "native" || environmentBackend === "overlay") return environmentBackend
    var value = wiggleSettings.backend || "native"
    return value === "overlay" ? "overlay" : "native"
  }

  Loader {
    id: backendLoader
    anchors.fill: parent
    source: root.configuredBackend === "overlay"
      ? Qt.resolvedUrl("Wiggle.qml")
      : Qt.resolvedUrl("backend/NativeBackend.qml")

    onLoaded: {
      if (!item) return
      if ("shell" in item) item.shell = root.shell
      if ("manifest" in item) item.manifest = root.manifest
      if ("settings" in item) item.settings = root.wiggleSettings.native || ({})
    }

    onStatusChanged: {
      if (status === Loader.Error)
        console.warn("wiggle: failed to load " + root.configuredBackend + " backend: " + errorString())
    }
  }
}

