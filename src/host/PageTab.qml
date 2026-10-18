import QtQuick
import QtQuick.Controls.Basic
TabButton {
    id: control
    implicitHeight: Theme.rowHeight
    implicitWidth: implicitContentWidth + 28
    font.pixelSize: Theme.compactText
    padding: 10
    contentItem: Text { text: control.text; font: control.font; color: control.checked ? Theme.text : Theme.muted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    background: Rectangle { radius: Theme.radius; color: control.checked ? Theme.selection : control.hovered ? Theme.hover : "transparent"; border.width: control.activeFocus ? 2 : 1; border.color: control.activeFocus ? Theme.accent : control.checked ? Theme.line : "transparent" }
}
