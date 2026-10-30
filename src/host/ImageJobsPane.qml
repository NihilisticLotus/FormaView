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
    readonly property var categories: Array.from(new Set(service.jobs.map(j => j.category || ""))).filter(c => !!c).sort()
    property string selectedCategory: ""
    readonly property var visibleJobs: service.jobs.filter(j => !selectedCategory || j.category === selectedCategory).slice().sort((a, b) => String(b.created_at || "").localeCompare(String(a.created_at || "")))
    function categoryLabel(value): string { return value === "model" ? Theme.t("模型与动画", "Models and animation", "模型與動畫") : value === "image" ? Theme.t("图片", "Images", "影像") : value }
    readonly property bool active: ["queued", "running"].includes(job.state)
    readonly property string firstOutput: (items.find(i => i.result && i.result.committed_output) || { result: {} }).result.committed_output || ""
    signal retry(string id)
    function count(state): int { return items.filter(i => i.status === state).length }
    readonly property int finished: items.filter(i => ["succeeded", "failed", "skipped", "cancelled", "interrupted"].includes(i.status)).length
    function syncJobSelection(): void { jobChoice.currentIndex = visibleJobs.findIndex(j => j.id === job.id) }
    spacing: 10
    Connections { target: root.service; function onChanged() { Qt.callLater(root.syncJobSelection) } }
    Component.onCompleted: syncJobSelection()
    Choice {
        id: categoryChoice; objectName: "jobCategoryFilter"; Layout.preferredWidth: 240
        model: [Theme.t("全部分类", "All categories", "全部分類")].concat(root.categories.map(c => root.categoryLabel(c)))
        currentIndex: root.categories.indexOf(root.selectedCategory) + 1
        onActivated: { root.selectedCategory = currentIndex > 0 ? root.categories[currentIndex - 1] : ""; if (root.visibleJobs.length) root.service.selectJob(root.visibleJobs[0].id); root.syncJobSelection() }
        Accessible.name: Theme.t("任务分类筛选", "Filter task category", "工作分類篩選")
    }
    Choice {
        id: jobChoice; objectName: "imageJobChoice"
        Layout.fillWidth: true; model: root.visibleJobs.map(j => (j.name || j.id) + " · " + root.categoryLabel(j.category || "") + " · " + Theme.state(j.state))
        onActivated: if (currentIndex >= 0 && currentIndex < root.visibleJobs.length) root.service.selectJob(root.visibleJobs[currentIndex].id)
        Accessible.name: Theme.t("转换任务", "Conversion job", "轉換工作")
    }
    Label { Layout.fillWidth: true; color: Theme.muted; textFormat: Text.PlainText; wrapMode: Text.WordWrap; text: root.job.id ? (root.job.created_at ? new Date(root.job.created_at).toLocaleString(Qt.locale()) + " · " : "") + root.job.id : "" }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text; text: Theme.state(root.job.state || "") + (root.job.cancellation_requested && root.active ? Theme.t(" · 取消中：等待当前可中断点", " · Cancelling: waiting for the current cancellation boundary", " · 取消中：等待目前可中斷點") : "") }
    Label { objectName: "imageGroupProgress"; visible: !!root.job.id; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("本次提交 · 已结束 %1 / %2 项", "This submission · %1 / %2 items finished", "本次提交 · 已結束 %1 / %2 項").arg(root.finished).arg(root.items.length) }
    ProgressBar {
        visible: !!root.job.id; Layout.fillWidth: true; from: 0; to: Math.max(1, root.items.length); value: root.finished
        background: Rectangle { implicitHeight: 4; radius: 2; color: Theme.field }
        contentItem: Item { implicitHeight: 4; Rectangle { width: parent.width * (root.finished / Math.max(1, root.items.length)); height: parent.height; radius: 2; color: Theme.accent } }
    }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("成功 %1 · 失败 %2 · 跳过 %3 · 取消 %4 · 中断 %5", "Success %1 · Failed %2 · Skipped %3 · Cancelled %4 · Interrupted %5", "成功 %1 · 失敗 %2 · 略過 %3 · 取消 %4 · 中斷 %5").arg(root.count("succeeded")).arg(root.count("failed")).arg(root.count("skipped")).arg(root.count("cancelled")).arg(root.count("interrupted")) }
    RowLayout {
        Layout.fillWidth: true; spacing: 24
        Label { Layout.fillWidth: true; Layout.preferredWidth: 30; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("来源 / 选择", "Source / Selection", "來源 / 選擇") }
        Label { Layout.fillWidth: true; Layout.preferredWidth: 32; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("输出 / 规则", "Output / Rules", "輸出 / 規則") }
        Label { Layout.fillWidth: true; Layout.preferredWidth: 38; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("结果", "Result", "結果") }
        Item { Layout.preferredWidth: 70 }
    }
    ListView {
        objectName: "imageJobsTable"
        Layout.fillWidth: true; Layout.fillHeight: true; model: root.items; clip: true; reuseItems: true; spacing: 6
        ScrollBar.vertical: ScrollBar {}
        delegate: ConversionItemRow {
            required property var modelData
            width: ListView.view.width; controller: root.controller; entry: modelData; preflight: false
        }
        Label { textFormat: Text.PlainText; anchors.centerIn: parent; visible: !root.job.id; color: Theme.muted; text: Theme.t("尚未提交转换任务", "No conversion job submitted", "尚未提交轉換工作") }
    }
    RowLayout {
        Tool { objectName: "cancelImageJob"; text: root.job.cancellation_requested ? Theme.t("取消中", "Cancelling", "取消中") : Theme.t("取消任务", "Cancel job", "取消工作"); visible: root.active; enabled: !root.job.cancellation_requested; onClicked: root.service.cancel("job", root.job.id) }
        Tool { text: Theme.t("未完成项重新预检…", "Preflight unfinished items…", "未完成項目重新預檢…"); enabled: !!root.job.id && !root.job.archived && !root.active && root.items.some(i => !["succeeded", "skipped"].includes(i.status)); onClicked: root.retry(root.job.id) }
        Tool { objectName: "revealJobOutput"; text: Theme.t("打开输出位置", "Show output location", "開啟輸出位置"); enabled: !!root.firstOutput; onClicked: root.controller.reveal(root.firstOutput) }
        Tool { text: Theme.t("复制任务报告", "Copy job report", "複製工作報告"); enabled: !!root.job.id; onClicked: root.controller.copy(JSON.stringify(root.job, null, 2)) }
    }
}
