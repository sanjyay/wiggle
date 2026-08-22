import QtQuick

QtObject {
  property string state: "checking"
  property string version: ""
  property string error: ""

  readonly property bool available: state === "available"
  readonly property bool missing: state === "unavailable"
  readonly property bool incompatible: state === "error"
}

