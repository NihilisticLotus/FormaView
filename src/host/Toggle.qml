pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
Switch {
    id: control
    implicitWidth: 44; implicitHeight: 32
    padding: 0
    focusPolicy: Qt.StrongFocus
    indicator: Rectangle {
        x: 0; y: (control.height - height) / 2; width: 44; height: 24; radius: 12
        color: control.checked ? Theme.action : Theme.line
        border.width: control.visualFocus ? 2 : 0; border.color: Theme.accent
        Rectangle { x: control.checked ? 22 : 2; y: 2; width: 20; height: 20; radius: 10; color: control.enabled ? "#ffffff" : Theme.tertiary }
    }
    contentItem: Item {}
}
