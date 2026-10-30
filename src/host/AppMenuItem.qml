import QtQuick
import QtQuick.Controls.Basic
MenuItem {
    id: control
    implicitHeight: Theme.rowHeight
    height: visible ? implicitHeight : 0
    implicitWidth: Math.max(220, implicitContentWidth + 54)
    font.pixelSize: Theme.compactText
    leftPadding: 28; rightPadding: 26
    contentItem: Text { text: control.text; font: control.font; color: control.enabled ? Theme.text : Theme.muted; opacity: control.enabled ? 1 : 0.55; elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter }
    indicator: AppIcon { x: 8; anchors.verticalCenter: parent.verticalCenter; visible: control.checked; name: "confirm"; size: 14 }
    arrow: AppIcon { x: control.width - 22; anchors.verticalCenter: parent.verticalCenter; visible: !!control.subMenu; name: "expand"; size: 16; variant: "muted" }
    background: Rectangle { color: control.highlighted ? Theme.selection : "transparent"; border.width: control.activeFocus ? 1 : 0; border.color: Theme.accent; radius: Theme.radius }
}
