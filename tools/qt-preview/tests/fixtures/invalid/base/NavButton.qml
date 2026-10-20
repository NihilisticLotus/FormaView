pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
Button {
    id: control
    property string iconName: "assets"
    property bool collapsed: false
    implicitHeight: 44
    font.pixelSize: 14
    leftPadding: collapsed ? 10 : 14; rightPadding: collapsed ? 10 : 12
    focusPolicy: Qt.StrongFocus
    contentItem: RowLayout {
        spacing: 12
        AppIcon { name: control.iconName; size: 20; variant: control.enabled ? "normal" : "disabled" }
        Text { visible: !control.collapsed; text: control.text; font: control.font; color: Theme.text; Layout.fillWidth: true; elide: Text.ElideRight }
    }
    background: Rectangle { radius: Theme.radius; color: control.checked ? Theme.selection : control.hovered ? Theme.panel : "transparent"; border.color: control.visualFocus ? Theme.accent : "transparent"; border.width: 2 }
    Accessible.name: text
    ToolTip.visible: hovered && collapsed
    ToolTip.text: text
}
