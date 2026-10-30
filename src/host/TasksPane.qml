pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Rectangle {
    id: root
    required property var controller
    signal back()
    signal retryImageJob(string id)
    readonly property bool hasImageJobs: !!controller.imageJobs && (controller.imageJobs.configured || controller.imageJobs.jobs.length > 0)
    color: Theme.base
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 20; spacing: 16
        RowLayout { Layout.fillWidth: true
            Label { Layout.fillWidth: true; text: Theme.t("任务中心", "Task center", "工作中心"); color: Theme.text; font.pixelSize: 24 }
            Tool { text: Theme.t("返回预览", "Back to preview", "返回預覽"); onClicked: root.back() }
        }
        Label { color: Theme.muted; text: Theme.t("活动任务：", "Active tasks: ", "活動工作：")+root.controller.activeTasks }
        TabBar { id: taskTabs; visible: root.hasImageJobs; Layout.fillWidth: true
            PageTab { text: Theme.t("转换任务", "Conversion jobs", "轉換工作") }
            PageTab { text: Theme.t("文件浏览", "File browsing", "檔案瀏覽") }
        }
        Loader {
            id: imageJobs; Layout.fillWidth: true; Layout.fillHeight: true
            active: root.visible && root.hasImageJobs && taskTabs.currentIndex === 0; visible: active
            onActiveChanged: { if (active) setSource("ImageJobsPane.qml", {controller: root.controller}) }
        }
        Connections { target: imageJobs.item; function onRetry(id) { root.retryImageJob(id) } }
        ListView {
            visible: !root.hasImageJobs || taskTabs.currentIndex === 1
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; reuseItems: true
            model: root.visible ? root.controller.tasks : null; spacing: 6
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: row
                required property var modelData
                width: ListView.view.width; height: details.implicitHeight+24; color: Theme.panel; radius: Theme.groupRadius
                RowLayout {
                    id: details
                    anchors.fill: parent; anchors.margins: 12; spacing: 20
                    ColumnLayout { Layout.fillWidth: true
                        Label { text: row.modelData.operation === "scan" ? Theme.t("目录扫描", "Directory scan", "資料夾掃描") : row.modelData.operation === "filter" ? Theme.t("排序与筛选", "Sort and filter", "排序與篩選") : Theme.t("文件读取检查", "File access check", "檔案讀取檢查"); color: Theme.text; font.pixelSize: 16 }
                        Label { Layout.fillWidth: true; text: row.modelData.path; color: Theme.muted; font.pixelSize: 13; elide: Text.ElideMiddle }
                        ExplainedText { objectName: "taskDetail"; Layout.fillWidth: true; kind: "error"; raw: row.modelData.detail }
                    }
                    Label { text: Theme.state(row.modelData.state); color: row.modelData.state === "failed" ? Theme.error : Theme.text }
                    Tool { visible: !row.modelData.terminal; enabled: !row.modelData.cancelRequested; text: Theme.t("取消", "Cancel", "取消"); onClicked: root.controller.cancel(row.modelData.id) }
                    Tool { visible: row.modelData.terminal && ["failed","cancelled"].includes(row.modelData.state); text: Theme.t("重新检查", "Check again", "重新檢查"); onClicked: root.controller.retry(row.modelData.id) }
                    Tool { text: Theme.t("复制详情", "Copy details", "複製詳情"); onClicked: root.controller.copy(JSON.stringify(row.modelData,null,2)) }
                }
            }
            Label { anchors.centerIn: parent; visible: root.controller.tasks.length === 0; text: Theme.t("暂无任务", "No tasks yet", "尚無工作"); color: Theme.muted }
        }
    }
}
