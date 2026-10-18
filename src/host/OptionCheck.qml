import QtQuick
import QtQuick.Controls.Basic
CheckBox {
    id: control
    implicitWidth: implicitContentWidth + 32
    implicitHeight: Math.max(Theme.controlHeight, implicitContentHeight + 12)
    spacing: 8; leftPadding: 0; rightPadding: 4; topPadding: 6; bottomPadding: 6
    font.pixelSize: Theme.compactText
    contentItem: Text { leftPadding: 28; text: control.text; font: control.font; color: control.enabled ? Theme.text : Theme.muted; wrapMode: Text.WordWrap; verticalAlignment: Text.AlignVCenter }
    indicator: Rectangle { x: 2; y: (control.height - height) / 2; width: 16; height: 16; radius: 3; color: control.checked ? Theme.action : Theme.field; border.color: control.checked ? Theme.action : Theme.line; AppIcon { anchors.centerIn: parent; visible: control.checked; name: "confirm"; variant: "on-action"; size: 14 } }
    background: Rectangle { color: control.hovered ? Theme.hover : "transparent"; border.width: control.activeFocus ? 2 : 0; border.color: Theme.accent; radius: Theme.radius }
}
