import QtQuick
import QtQuick.Controls.Basic
ApplicationWindow {
    id: root
    required property var controller
    width: 1600; height: 900
    minimumWidth: 1280; minimumHeight: 800
    title: "FormaView"
    color: Theme.base
    font.pixelSize: 14
    palette.window: Theme.panel
    palette.windowText: Theme.text
    palette.base: Theme.field
    palette.text: Theme.text
    palette.button: Theme.raised
    palette.buttonText: Theme.text
    palette.highlight: Theme.selection
    palette.highlightedText: Theme.text
    palette.mid: Theme.line
    Binding { target: Theme; property: "language"; value: root.controller.language }
    Workspace { anchors.fill: parent; anchors.margins: 4; controller: root.controller }
}
