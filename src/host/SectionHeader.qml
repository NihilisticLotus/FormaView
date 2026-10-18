pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
Button {
    id: control
    implicitHeight: Theme.rowHeight
    implicitWidth: implicitContentWidth + 28
    checkable: true; focusPolicy: Qt.StrongFocus
    font.pixelSize: Theme.compactText
    leftPadding: 10; rightPadding: 10
    contentItem: RowLayout {
        spacing: 8
        AppIcon { name: control.checked ? "collapse" : "expand"; size: 16; variant: control.enabled ? "normal" : "disabled" }
        Text { text: control.text; font: control.font; color: control.enabled ? Theme.text : Theme.muted; elide: Text.ElideRight; Layout.fillWidth: true }
    }
    background: Rectangle { radius: Theme.radius; color: control.down ? Theme.field : control.hovered ? Theme.hover : "transparent"; border.color: control.activeFocus ? Theme.accent : "transparent"; border.width: 2 }
}
