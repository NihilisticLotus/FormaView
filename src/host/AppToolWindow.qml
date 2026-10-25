pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
ApplicationWindow {
    id: root
    required property var controller
    property real nativeCaptionInset: 64
    flags: Qt.Tool | Qt.CustomizeWindowHint | Qt.WindowSystemMenuHint | Qt.WindowCloseButtonHint | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    modality: Qt.NonModal
    color: Theme.base
    font.pixelSize: Theme.formText
    font.family: controller.language === "en" ? "Segoe UI Variable Text" : "Microsoft YaHei UI"
    palette.window: Theme.panel
    palette.windowText: Theme.text
    palette.base: Theme.field
    palette.text: Theme.text
    palette.button: Theme.raised
    palette.buttonText: Theme.text
    palette.highlight: Theme.selection
    palette.highlightedText: Theme.text
    palette.mid: Theme.line
    topPadding: 0; bottomPadding: 0; leftPadding: 0; rightPadding: 0
    header: Rectangle {
        objectName: "topBar"; height: 52; color: Theme.base
        Item { objectName: "windowTitleDrag"; anchors.fill: parent }
        Label { anchors.left: parent.left; anchors.leftMargin: 24; anchors.right: parent.right; anchors.rightMargin: Math.max(64, root.nativeCaptionInset); anchors.verticalCenter: parent.verticalCenter; font.pixelSize: Theme.formHeading; color: Theme.text; text: root.title; elide: Text.ElideRight }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
    }
    Shortcut { sequence: "Escape"; context: Qt.WindowShortcut; onActivated: root.close() }
    Component.onCompleted: controller.styleToolWindow(root)
}
