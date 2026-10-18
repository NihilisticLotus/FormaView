pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
MenuBar {
    id: root
    required property var controller
    signal openFile()
    signal openFolder()
    signal toggleFiles()
    signal toggleFocus()
    signal tasks()
    signal settings()
    signal unavailable()
    Menu {
        title: Theme.t("文件", "File", "檔案")
        MenuItem { text: Theme.t("打开文件…", "Open file…", "開啟檔案…"); onTriggered: root.openFile() }
        MenuItem { text: Theme.t("打开目录…", "Open folder…", "開啟資料夾…"); onTriggered: root.openFolder() }
        Menu {
            id: recentMenu
            title: Theme.t("最近目录", "Recent folders", "最近資料夾")
            enabled: root.controller.recent.length > 0
            Instantiator {
                model: root.controller.recent
                delegate: MenuItem { required property string modelData; text: modelData; onTriggered: root.controller.navigate(modelData) }
                onObjectAdded: (index, object) => recentMenu.insertItem(index,object)
                onObjectRemoved: (index, object) => recentMenu.removeItem(object)
            }
        }
        MenuItem { text: Theme.t("刷新目录", "Refresh folder", "重新整理資料夾"); enabled: root.controller.directory.length>0; onTriggered: root.controller.refresh() }
        MenuItem { text: Theme.t("关闭文件", "Close file", "關閉檔案"); enabled: !!root.controller.asset.path; onTriggered: root.controller.clearAsset() }
        MenuSeparator {}
        MenuItem { text: Theme.t("退出", "Quit", "結束"); onTriggered: Qt.quit() }
    }
    Menu { title: Theme.t("查看", "View", "檢視")
        MenuItem { text: Theme.t("文件区", "Files pane", "檔案區"); onTriggered: root.toggleFiles() }
        MenuItem { text: Theme.t("专注预览", "Focus view", "專注預覽"); onTriggered: root.toggleFocus() }
        MenuItem { text: Theme.t("任务中心", "Task center", "工作中心"); onTriggered: root.tasks() }
    }
    Menu { title: Theme.t("模型", "Model", "模型"); MenuItem { text: Theme.t("模型检查尚未接入", "Model inspection is not connected", "模型檢查尚未接入"); enabled: false } }
    Menu { title: Theme.t("动画", "Animation", "動畫"); MenuItem { text: Theme.t("外部动画…", "External animation…", "外部動畫…"); onTriggered: root.unavailable() } }
    Menu { title: Theme.t("图片", "Image", "影像"); MenuItem { text: Theme.t("专业图片预览尚未接入", "Professional image preview is not connected", "專業影像預覽尚未接入"); enabled: false } }
    Menu { title: Theme.t("工具", "Tools", "工具")
        MenuItem { text: Theme.t("任务中心", "Task center", "工作中心"); onTriggered: root.tasks() }
        MenuItem { text: Theme.t("MCP 设置…", "MCP settings…", "MCP 設定…"); onTriggered: root.settings() }
    }
    Menu { title: Theme.t("帮助", "Help", "說明"); MenuItem { text: Theme.t("当前可用功能", "Available features", "目前可用功能"); onTriggered: root.unavailable() } }
}
