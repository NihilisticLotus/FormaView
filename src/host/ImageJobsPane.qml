pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
    required property var controller
    readonly property var service: controller.imageJobs
    readonly property var job: service.job
    readonly property var items: job.items || []
    readonly property bool active: ["queued", "running"].includes(job.state)
    signal retry(string id)
    function count(state): int { return items.filter(i => i.status === state).length }
    spacing: 10
    ComboBox {
        Layout.fillWidth: true; model: root.service.jobs.map(j => j.id + " · " + Theme.state(j.state))
        currentIndex: root.service.jobs.findIndex(j => j.id === root.job.id)
        onActivated: root.service.selectJob(root.service.jobs[currentIndex].id)
        Accessible.name: Theme.t("图片任务", "Image job", "影像工作")
    }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text; text: Theme.state(root.job.state || "") + (root.job.cancellation_requested && root.active ? Theme.t(" · 取消中：等待当前可中断点", " · Cancelling: waiting for the current cancellation boundary", " · 取消中：等待目前可中斷點") : "") }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("成功 %1 · 失败 %2 · 跳过 %3 · 取消 %4 · 中断 %5", "Success %1 · Failed %2 · Skipped %3 · Cancelled %4 · Interrupted %5", "成功 %1 · 失敗 %2 · 略過 %3 · 取消 %4 · 中斷 %5").arg(root.count("succeeded")).arg(root.count("failed")).arg(root.count("skipped")).arg(root.count("cancelled")).arg(root.count("interrupted")) }
    ListView {
        Layout.fillWidth: true; Layout.fillHeight: true; model: root.items; clip: true; reuseItems: true; spacing: 6
        ScrollBar.vertical: ScrollBar {}
        delegate: Rectangle {
            id: row; required property var modelData
            width: ListView.view.width; implicitHeight: details.implicitHeight + 24; color: Theme.panel
            ColumnLayout {
                id: details; anchors.fill: parent; anchors.margins: 12
                Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.text; text: row.modelData.request.input + "\n→ " + (row.modelData.result?.committed_output || row.modelData.request.output) }
                Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: row.modelData.status === "failed" ? Theme.error : Theme.accent; text: Theme.state(row.modelData.status) + (row.modelData.error ? " · " + row.modelData.error.code + ": " + row.modelData.error.message : "") }
                Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.error; visible: !!row.modelData.temporary_cleanup_error || !!row.modelData.outcome_uncertain; text: Theme.t("需检查中断或临时文件清理结果，详情包含实际路径。", "Review interrupted output or temporary-file cleanup; details include actual paths.", "需檢查中斷或暫存檔清理結果，詳情包含實際路徑。") }
                Tool { text: Theme.t("复制详情", "Copy details", "複製詳情"); onClicked: root.controller.copy(JSON.stringify(row.modelData, null, 2)) }
            }
        }
        Label { textFormat: Text.PlainText; anchors.centerIn: parent; visible: !root.job.id; color: Theme.muted; text: Theme.t("尚未提交图片任务", "No image job submitted", "尚未提交影像工作") }
    }
    RowLayout {
        Tool { text: root.job.cancellation_requested ? Theme.t("取消中", "Cancelling", "取消中") : Theme.t("取消任务", "Cancel job", "取消工作"); visible: root.active; enabled: !root.job.cancellation_requested; onClicked: root.service.cancel("job", root.job.id) }
        Tool { text: Theme.t("未完成项重新预检…", "Preflight unfinished items…", "未完成項目重新預檢…"); enabled: !!root.job.id && !root.active && root.items.some(i => !["succeeded", "skipped"].includes(i.status)); onClicked: root.retry(root.job.id) }
        Tool { text: Theme.t("复制任务报告", "Copy job report", "複製工作報告"); enabled: !!root.job.id; onClicked: root.controller.copy(JSON.stringify(root.job, null, 2)) }
    }
}
