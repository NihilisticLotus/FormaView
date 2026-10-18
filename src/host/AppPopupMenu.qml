import QtQuick
import QtQuick.Controls.Basic
Menu {
    popupType: Popup.Item
    padding: 4
    implicitWidth: Math.max(240, implicitContentWidth + 8)
    delegate: AppMenuItem {}
    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: Theme.radius }
}
