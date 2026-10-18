import QtQuick
import QtQuick.Controls.Basic
Button {
    id: control
    property string hint: ""
    implicitHeight: 34
    font.pixelSize: 14
    focusPolicy: Qt.StrongFocus
    leftPadding: 10
    rightPadding: 10
    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.muted : control.checked ? Theme.accent : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 5
        color: control.down ? Theme.field : control.checked ? Theme.selection : control.hovered ? "#414347" : Theme.raised
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? Theme.accent : Theme.line
        opacity: control.enabled ? 1 : 0.65
    }
    ToolTip.visible: hovered && hint.length > 0
    ToolTip.text: hint
    ToolTip.delay: 600
    Accessible.description: hint
}
