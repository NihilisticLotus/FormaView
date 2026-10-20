import QtQuick
import QtQuick.Controls.Basic
TextField {
    id: control
    property bool form: false
    implicitHeight: control.form ? Theme.formControlHeight : Theme.controlHeight
    font.pixelSize: control.form ? Theme.formText : Theme.compactText
    color: Theme.text
    selectionColor: Theme.selection
    selectedTextColor: Theme.text
    placeholderTextColor: Theme.muted
    selectByMouse: true
    background: Rectangle {
        radius: Theme.radius
        color: Theme.field
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? Theme.accent : Theme.line
    }
}
