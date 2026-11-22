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
    readonly property var mainDestinations: ["assets", "conversion", "tasks"]
    readonly property var settingsDestinations: ["general", "preview", "video", "export", "cache", "permissions", "keys", "about"]
    function destinationLabel(destination): string {
        switch (destination) {
        case "assets": return Theme.t("资产", "Assets", "資產")
        case "conversion": return Theme.t("转换", "Convert", "轉換")
        case "tasks": return Theme.t("任务", "Tasks", "工作")
        case "general": return Theme.t("通用与外观", "General & appearance", "一般與外觀")
        case "preview": return Theme.t("预览与窗口", "Preview & windows", "預覽與視窗")
        case "video": return Theme.t("视频播放","Video playback","影片播放")
        case "export": return Theme.t("导出默认值","Export defaults","匯出預設值")
        case "keys": return Theme.t("快捷键","Keyboard shortcuts","快速鍵")
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
    Keys.onEscapePressed: event => { if (page === "settings") { event.accepted = false } }
    color: Theme.light ? "#4cffffff" : "#36243241"
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 8; spacing: 6
        RowLayout {
            visible: root.page !== "settings"; Layout.preferredHeight: 44; Layout.leftMargin: root.collapsed ? 0 : 12; Layout.alignment: root.collapsed ? Qt.AlignHCenter : Qt.AlignLeft; spacing: 10
            Image { objectName: "navigationBrand"; source: "qrc:/formaview/icons/formaview.png"; Layout.preferredWidth: 28; Layout.preferredHeight: 28; sourceSize: Qt.size(28 * Screen.devicePixelRatio, 28 * Screen.devicePixelRatio); Accessible.ignored: true }
            Label { visible: !root.collapsed; text: "FormaView"; font.pixelSize: 18; font.bold: true; color: Theme.text }
        }
        Item { Layout.preferredHeight: 10 }
        Repeater {
            id: navigationItems
            model: root.page === "settings" ? root.settingsDestinations : root.mainDestinations
            NavButton { required property string modelData; objectName: modelData === "conversion" ? "convertImagesButton" : modelData === "tasks" ? "tasksButton" : "nav-" + modelData; Layout.fillWidth: true; enabled: modelData !== "conversion" || root.conversionAvailable; iconName: ({"video":"animation-play","export":"conversion","keys":"help"})[modelData] || modelData; text: root.destinationLabel(modelData); collapsed: root.collapsed; checked: root.page === "settings" ? root.section === modelData : root.page === modelData; onClicked: root.navigate(modelData) }
        }
        Item { Layout.fillHeight: true }
        NavButton { visible: root.page !== "settings"; objectName: "nav-settings"; Layout.fillWidth: true; iconName: "settings"; collapsed: root.collapsed; text: Theme.t("设置", "Settings", "設定"); onClicked: root.navigate("settings") }
    }
    Rectangle { objectName: "navigationDivider"; anchors.right: root.right; height: root.height; width: 1; color: Theme.line }
}
