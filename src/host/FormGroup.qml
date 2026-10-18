import QtQuick
import QtQuick.Controls.Basic
GroupBox {
    id: control
    implicitWidth: 0
    padding: 20; topPadding: 52
    font.pixelSize: Theme.bodyText
    label: Text { x: 0; y: 0; text: control.title; font.pixelSize: 15; font.bold: true; color: Theme.text }
    background: Rectangle { y: 32; height: control.height - 32; color: Theme.raised; border.color: Theme.line; radius: Theme.groupRadius }
}
