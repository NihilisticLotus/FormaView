import QtQuick
import QtQuick.Controls.Basic
GroupBox {
    id: control
    implicitWidth: 0
    padding: 20; topPadding: 52
    font.pixelSize: Theme.formText
    label: Text { x: 0; y: 0; text: control.title; font.pixelSize: Theme.formHeading; font.bold: true; color: Theme.text }
    background: Rectangle { y: 32; height: control.height - 32; gradient: Gradient { GradientStop { position: 0; color: Theme.raised } GradientStop { position: 1; color: Theme.panel } }
        border.color: Theme.line; radius: Theme.groupRadius }
}
