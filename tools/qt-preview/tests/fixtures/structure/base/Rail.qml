pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
Rectangle {
    id: root
    required property string page
    property string section: "general"
    property bool collapsed: false
    property bool conversionAvailable: true
    readonly property var mainDestinations: [{key:"assets"}, {key:"conversion"}, {key:"tasks"}]
    readonly property var settingsDestinations: [{key:"general"}, {key:"preview"}, {key:"cache"}, {key:"permissions"}, {key:"about"}]
    function destinationLabel(destination): string {
        switch (destination) {
        case "assets": return Theme.t("资产", "Assets", "資產")
        case "conversion": return Theme.t("转换", "Convert", "轉換")
        case "tasks": return Theme.t("任务", "Tasks", "工作")
        case "general": return Theme.t("通用", "General", "一般")
        case "preview": return Theme.t("预览", "Preview", "預覽")
        case "cache": return Theme.t("缓存", "Cache", "快取")
        case "permissions": return Theme.t("MCP 与权限", "MCP & permissions", "MCP 與權限")
        case "about": return Theme.t("关于", "About", "關於")
        }
        return ""
    }
    signal navigate(string destination)
    function focusDestination(destination): void {
        for (let index = 0; index < navigationItems.count; ++index) {
            const button = navigationItems.itemAt(index)
            if (button && navigationItems.model[index] === destination) { button.forceActiveFocus(); return }
        }
    }
    Keys.onEscapePressed: event => { if (page === "settings") { navigate("assets"); event.accepted = true } }
    color: Theme.sidebar
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 8; spacing: 6
        RowLayout {
            Layout.preferredHeight: 44; Layout.leftMargin: 12; spacing: 10
            AppIcon { name: "brand-diamond"; size: 24 }
            Label { visible: !root.collapsed; text: "FormaView"; font.pixelSize: 18; font.bold: true; color: Theme.text }
        }
        Item { Layout.preferredHeight: 10 }
        NavButton { visible: root.page === "settings"; Layout.fillWidth: true; iconName: "return"; collapsed: root.collapsed; text: Theme.t("返回资产", "Back to assets", "返回資產"); onClicked: root.navigate("assets") }
        Repeater {
            id: navigationItems
            model: root.page === "settings" ? root.settingsDestinations : root.mainDestinations
            NavButton { required property var modelData; objectName: modelData.key === "conversion" ? "convertImagesButton" : modelData.key === "tasks" ? "tasksButton" : "nav-" + modelData.key; Layout.fillWidth: true; enabled: modelData.key !== "conversion" || root.conversionAvailable; iconName: modelData.key; text: root.destinationLabel(modelData.key); collapsed: root.collapsed; checked: root.page === "settings" ? root.section === modelData.key : root.page === modelData.key; onClicked: root.navigate(modelData.key) }
        }
        Item { Layout.fillHeight: true }
        NavButton { visible: root.page !== "settings"; objectName: "nav-settings"; Layout.fillWidth: true; iconName: "settings"; collapsed: root.collapsed; text: Theme.t("设置", "Settings", "設定"); onClicked: root.navigate("settings") }
    }
    Rectangle { anchors.right: root.right; height: root.height; width: 1; color: Theme.line }
}
