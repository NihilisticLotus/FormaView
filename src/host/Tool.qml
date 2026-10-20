pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
Button {
    id: control
    property bool form: false
    property bool quiet: false
    property bool primary: false
    property string hint: ""
    property string iconName: ""
    property real iconRotation: 0
    implicitHeight: control.form ? Theme.formControlHeight : Theme.controlHeight
    implicitWidth: Math.max(32, implicitContentWidth + leftPadding + rightPadding)
    font.pixelSize: control.form ? Theme.formText : Theme.compactText
    focusPolicy: Qt.StrongFocus
    leftPadding: text ? 12 : 7
    rightPadding: leftPadding
    contentItem: RowLayout {
        spacing: 8
        AppIcon { visible: !!control.iconName; name: control.iconName; rotation: control.iconRotation; variant: !control.enabled ? "disabled" : control.primary ? "on-action" : "normal"; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
        Text { visible: !!control.text; text: control.text; font: control.font; color: !control.enabled ? Theme.tertiary : control.primary ? "#ffffff" : Theme.text; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; Layout.fillWidth: true }
    }
    background: Rectangle {
        radius: Theme.radius
        color: control.primary ? (control.down ? Theme.actionPressed : control.hovered ? Theme.actionHover : Theme.action) : control.down ? Theme.field : control.hovered ? Theme.hover : control.checked ? Theme.selection : control.quiet ? "transparent" : Theme.raised
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? Theme.accent : control.primary || control.quiet ? "transparent" : Theme.line
        opacity: control.enabled ? 1 : 0.45
    }
    ToolTip.visible: hovered && hint.length > 0
    ToolTip.text: hint
    ToolTip.delay: 600
    Accessible.description: hint
    Accessible.name: text || hint
}
