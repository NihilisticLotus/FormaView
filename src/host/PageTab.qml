import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
TabButton {
    id: control
    property bool stepStyle: false
    property bool lastStep: false
    implicitHeight: stepStyle ? 32 : Theme.rowHeight
    implicitWidth: implicitContentWidth + 28
    font.pixelSize: Theme.compactText
    padding: stepStyle ? 4 : 10
    contentItem: RowLayout {
        spacing: 16
        Text { Layout.fillWidth: true; text: control.text; font: control.font; color: control.checked ? (control.stepStyle ? Theme.accent : Theme.text) : Theme.muted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        Text { visible: control.stepStyle && !control.lastStep; text: "›"; color: Theme.muted; font.pixelSize: 18; Accessible.ignored: true }
    }
    background: Rectangle {
        radius: control.stepStyle ? Theme.radius : 9
        color: control.checked && !control.stepStyle ? (Theme.light ? "#c6ffffff" : "#80576c7c") : control.hovered ? Theme.hover : "transparent"
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? Theme.accent : "transparent"
    }
}
