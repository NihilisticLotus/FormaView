import QtQuick
import QtQuick.Controls.Basic
ApplicationWindow {
    id: root
    required property var controller
    property int frostDepth: 0
    width: 1600; height: 900
    property real nativeCaptionInset: 160
    property real nativeCaptionHeight: 32
    minimumWidth: 760; minimumHeight: 580
    title: "FormaView"
    flags: Qt.Window | Qt.CustomizeWindowHint | Qt.WindowSystemMenuHint | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0
    color: Theme.base
    font.pixelSize: Theme.bodyText
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
    Binding { target: Theme; property: "language"; value: root.controller.language }
    Binding { target: Theme; property: "reduceMotion"; value: controller.preferences.reduceMotion === true }
    Binding { target: Theme; property: "compact"; value: root.controller.preferences.density === "compact" }
    onActiveChanged: { if (active && controller) controller.reloadLibrary() }
    onClosing: close => { close.accepted = workspace.requestWindowClose() }
    Workspace { id: workspace; anchors.fill: parent; controller: root.controller }
    Rectangle {
        objectName: "nativeCaptionBackdrop"
        anchors.top: parent.top; anchors.right: parent.right
        width: root.nativeCaptionInset - 8; height: root.nativeCaptionHeight
        bottomLeftRadius: 10; color: "#526a7d"
        visible: Qt.platform.os === "windows" && Theme.light && root.visibility !== Window.FullScreen
                 && !!(root.flags & Qt.ExpandedClientAreaHint) && !(root.flags & Qt.FramelessWindowHint)
        Accessible.ignored: true
    }
}
