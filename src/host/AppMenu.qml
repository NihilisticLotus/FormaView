pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
MenuBar {
    id: root
    background: Rectangle { color: "transparent" }
    delegate: MenuBarItem {
        id: entry
        objectName: "appMenuEntry"
        font.pixelSize: Theme.compactText
        implicitHeight: Theme.controlHeight
        leftPadding: 10; rightPadding: 10
        contentItem: Text { text: entry.text; font: entry.font; color: Theme.text; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: Theme.radius; color: entry.highlighted ? Theme.selection : "transparent"; border.width: entry.activeFocus ? 2 : 0; border.color: Theme.accent }
    }
    required property var controller
    signal openFile()
    signal toggleFiles()
    signal toggleFocus()
    signal tasks()
    signal settings()
    signal unavailable()
    signal convert()
    AppPopupMenu {
        title: Theme.t("文件", "File", "檔案")
        AppMenuItem { text: Theme.t("打开文件…", "Open file…", "開啟檔案…"); onTriggered: root.openFile() }
        AppPopupMenu {
            id: recentMenu
            title: Theme.t("最近目录", "Recent folders", "最近資料夾")
            enabled: root.controller.recent.length > 0
            Instantiator {
                model: root.controller.recent
                delegate: AppMenuItem { required property string modelData; text: modelData; onTriggered: root.controller.navigate(modelData) }
                onObjectAdded: (index, object) => recentMenu.insertItem(index,object)
                onObjectRemoved: (index, object) => recentMenu.removeItem(object)
            }
        }
        AppMenuItem { text: Theme.t("刷新目录", "Refresh folder", "重新整理資料夾"); enabled: root.controller.directory.length>0; onTriggered: root.controller.refresh() }
        AppMenuItem { text: Theme.t("关闭文件", "Close file", "關閉檔案"); enabled: !!root.controller.asset.path; onTriggered: root.controller.clearAsset() }
        MenuSeparator {}
        AppMenuItem { text: Theme.t("退出", "Quit", "結束"); onTriggered: Qt.quit() }
    }
    AppPopupMenu { title: Theme.t("查看", "View", "檢視")
        AppMenuItem { text: Theme.t("文件区", "Files pane", "檔案區"); onTriggered: root.toggleFiles() }
        AppMenuItem { text: Theme.t("专注预览", "Focus view", "專注預覽"); onTriggered: root.toggleFocus() }
        AppMenuItem { text: Theme.t("任务中心", "Task center", "工作中心"); onTriggered: root.tasks() }
    }
    AppPopupMenu {
        title: Theme.t("模型", "Model", "模型")
        enabled: !!root.controller.model && root.controller.model.available
        AppMenuItem { text: Theme.t("模型", "Model", "模型"); onTriggered: root.controller.model.setMode(0) }
        AppMenuItem { text: Theme.t("UV 检查", "UV inspection", "UV 檢查"); onTriggered: root.controller.model.setMode(1) }
        AppMenuItem { text: Theme.t("源权重", "Source weights", "來源權重"); onTriggered: root.controller.model.setMode(2) }
        AppMenuItem { text: Theme.t("LOD / 碰撞", "LOD / Collision", "LOD / 碰撞"); onTriggered: root.controller.model.setMode(3) }
    }
    AppPopupMenu { title: Theme.t("动画", "Animation", "動畫"); AppMenuItem { text: Theme.t("外部动画…", "External animation…", "外部動畫…"); onTriggered: root.unavailable() } }
    AppPopupMenu { title: Theme.t("图片", "Image", "影像")
        AppMenuItem { text: Theme.t("打开图片…", "Open image…", "開啟影像…"); enabled: !!root.controller.image; onTriggered: root.openFile() }
        AppMenuItem { text: Theme.t("转换选定图片…", "Convert selected images…", "轉換選定影像…"); enabled: !!root.controller.imageJobs; onTriggered: root.convert() }
    }
    AppPopupMenu { title: Theme.t("工具", "Tools", "工具")
        AppMenuItem { text: Theme.t("任务中心", "Task center", "工作中心"); onTriggered: root.tasks() }
        AppMenuItem { text: Theme.t("MCP 设置…", "MCP settings…", "MCP 設定…"); onTriggered: root.settings() }
    }
    AppPopupMenu { title: Theme.t("帮助", "Help", "說明"); AppMenuItem { text: Theme.t("当前可用功能", "Available features", "目前可用功能"); onTriggered: root.unavailable() } }
}
