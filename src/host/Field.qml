import QtQuick
import QtQuick.Controls.Basic
TextField {
    id: control
    implicitHeight: 34
    font.pixelSize: 14
    color: Theme.text
    selectionColor: Theme.selection
    selectedTextColor: Theme.text
    placeholderTextColor: Theme.muted
    selectByMouse: true
    background: Rectangle {
        radius: 5
        color: Theme.field
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.accent : Theme.line
    }
}
