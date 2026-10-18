import QtQuick
import QtQuick.Controls.Basic
SpinBox {
    id: control
    implicitWidth: 140
    implicitHeight: Theme.controlHeight
    font.pixelSize: Theme.compactText
    up.indicator: Rectangle {
        x: control.mirrored ? 0 : control.width - width
        implicitWidth: 30; implicitHeight: Theme.controlHeight; height: control.height
        color: control.up.pressed ? Theme.field : control.up.hovered ? Theme.hover : Theme.raised
        AppIcon { anchors.centerIn: parent; name: "collapse"; rotation: 180; size: 16; variant: control.up.indicator.enabled ? "normal" : "disabled" }
    }
    down.indicator: Rectangle {
        x: control.mirrored ? control.width - width : 0
        implicitWidth: 30; implicitHeight: Theme.controlHeight; height: control.height
        color: control.down.pressed ? Theme.field : control.down.hovered ? Theme.hover : Theme.raised
        AppIcon { anchors.centerIn: parent; name: "collapse"; size: 16; variant: control.down.indicator.enabled ? "normal" : "disabled" }
    }
    background: Rectangle { color: Theme.field; border.color: control.activeFocus ? Theme.accent : Theme.line; border.width: control.activeFocus ? 2 : 1; radius: Theme.radius }
}
