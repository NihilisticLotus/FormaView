pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
ColumnLayout {
    id: root
    required property var controller
    readonly property var service: controller.imageJobs
    property string fixedJobId: ""
    property bool compact: false
    property bool awaitingJob: false
    property bool failuresOnly: false
    Label {
        visible: root.service.busy && !root.job.id
        Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted
        text: Theme.t("正在读取片段列表，随后逐项检查并导出…", "Reading the clip list, then checking and exporting each item…", "正在讀取片段清單，隨後逐項檢查並匯出…")
    }
    property int snapshotRevision: 0
    // Poll notifications refresh a pinned job without changing the main task selection.
    readonly property var job: {
        const current = service.job
        const rows = service.jobs
        const revision = snapshotRevision
        return awaitingJob ? ({}) : !fixedJobId || current.id === fixedJobId ? current : service.jobSnapshot(fixedJobId)
    }
    Timer {
        interval: 200; repeat: true
        running: root.compact && root.visible && !!root.Window.window?.visible && root.active && !!root.fixedJobId && root.service.job.id !== root.fixedJobId
        onTriggered: root.snapshotRevision++
    }
    readonly property var items: job.items || []
    readonly property var shownItems: failuresOnly ? items.filter(i => i.status === "failed" || i.status === "interrupted") : items
    readonly property var categories: Array.from(new Set(service.jobs.map(j => j.category || ""))).filter(c => !!c).sort()
    property string selectedCategory: ""
    readonly property var visibleJobs: service.jobs.filter(j => !selectedCategory || j.category === selectedCategory).slice().sort((a, b) => String(b.created_at || "").localeCompare(String(a.created_at || "")))
    function categoryLabel(value): string { return value === "model" ? Theme.t("模型与动画", "Models and animation", "模型與動畫") : value === "image" ? Theme.t("图片", "Images", "影像") : value }
    function batchKey(value): string { return String(value || "").replace(/-job$/, "").slice(-8) }
    readonly property bool active: ["queued", "running", "paused", "pausing"].includes(job.state)
    readonly property string firstOutput: (items.find(i => i.result && i.result.committed_output) || { result: {} }).result.committed_output || ""
    signal retry(string id)
    function count(state): int { return items.filter(i => i.status === state).length }
    readonly property int finished: items.filter(i => ["succeeded", "failed", "skipped", "cancelled", "interrupted"].includes(i.status)).length
    function syncJobSelection(): void {
        if (compact) return
        jobChoice.currentIndex = visibleJobs.findIndex(j => j.id === job.id)
        if (batches.currentIndex !== jobChoice.currentIndex) {
            batches.currentIndex = jobChoice.currentIndex
            if (batches.currentIndex >= 0) batches.positionViewAtIndex(batches.currentIndex, ListView.Contain)
        }
    }
    spacing: 10
    ExplainedText { Layout.fillWidth: true; visible: !!root.service.error; kind: "error"; raw: root.service.error }
    ExplainedText {
        objectName: "imageJobError"
        Layout.fillWidth: true; kind: "error"
        raw: root.job.error ? (root.job.error.code || "error") + ": " + (root.job.error.message || "") : ""
    }
    Connections { target: root.service; function onChanged() { Qt.callLater(root.syncJobSelection) } }
    Component.onCompleted: syncJobSelection()
    RowLayout {
      visible: !root.compact; Layout.fillWidth: true
      Label { text: Theme.t("提交批次", "Submission batches", "提交批次"); color: Theme.text; font.bold: true }
      Item { Layout.fillWidth: true }
      Choice {
        id: categoryChoice; objectName: "jobCategoryFilter"; Layout.preferredWidth: 240
        model: [Theme.t("全部分类", "All categories", "全部分類")].concat(root.categories.map(c => root.categoryLabel(c)))
        currentIndex: root.categories.indexOf(root.selectedCategory) + 1
        onActivated: { root.selectedCategory = currentIndex > 0 ? root.categories[currentIndex - 1] : ""; if (root.visibleJobs.length) root.service.selectJob(root.visibleJobs[0].id); root.syncJobSelection() }
        Accessible.name: Theme.t("任务分类筛选", "Filter task category", "工作分類篩選")
      }
    }
    Choice {
        id: jobChoice; objectName: "imageJobChoice"
        visible: !root.compact
        Layout.fillWidth: true; model: root.visibleJobs.map(j => (j.name || j.id) + " · " + root.categoryLabel(j.category || "") + " · " + Theme.state(j.state))
        onActivated: if (currentIndex >= 0 && currentIndex < root.visibleJobs.length) root.service.selectJob(root.visibleJobs[currentIndex].id)
        Accessible.name: Theme.t("转换任务", "Conversion job", "轉換工作")
    }
    ListView {
        id: batches; objectName: "conversionBatchList"
        Layout.fillWidth: true; Layout.preferredHeight: visible ? 118 : 0
        visible: !root.compact && count > 0; orientation: ListView.Horizontal
        model: root.visibleJobs; spacing: 10; clip: true; reuseItems: true
        ScrollBar.horizontal: ScrollBar { policy: batches.contentWidth > batches.width ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded }
        delegate: Button {
            id: batch
            required property var modelData
            width: Math.min(340, batches.width); height: 100
            objectName: "conversionBatch_" + modelData.id
            checked: root.job.id === modelData.id
            onClicked: root.service.selectJob(modelData.id)
            padding: 12
            Accessible.name: modelData.name || modelData.id
            Accessible.description: details.text + " · " + state.text
            ToolTip.visible: hovered; ToolTip.text: (modelData.name || modelData.id) + "\n" + modelData.id
            background: Rectangle {
                radius: Theme.radius; color: batch.checked ? Theme.selection : batch.hovered ? Theme.hover : Theme.panel
                border.width: batch.checked || batch.visualFocus ? 2 : 1
                border.color: batch.checked || batch.visualFocus ? Theme.accent : Theme.line
            }
            contentItem: ColumnLayout {
                spacing: 6
                Label { Layout.fillWidth: true; text: batch.modelData.name || batch.modelData.id; textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.text; font.bold: true }
                Label {
                    id: details; Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.muted; textFormat: Text.PlainText
                    text: (batch.modelData.created_at ? new Date(batch.modelData.created_at).toLocaleString(Qt.locale(), "MM-dd HH:mm:ss") : "—") + " · " + Theme.t("%1 项", "%1 items", "%1 項").arg(batch.modelData.total !== undefined ? batch.modelData.total : (batch.modelData.items || []).length)
                }
                Label { id: state; Layout.fillWidth: true; elide: Text.ElideRight; textFormat: Text.PlainText; color: batch.checked ? Theme.accent : Theme.muted; text: root.categoryLabel(batch.modelData.category || "") + " · " + Theme.state(batch.modelData.state) + " · #" + root.batchKey(batch.modelData.id) }
            }
        }
    }
    Label { objectName: "selectedBatchHeading"; visible: !!root.job.id; Layout.fillWidth: true; color: Theme.text; font.bold: true; textFormat: Text.PlainText; wrapMode: Text.WordWrap; text: Theme.t("批次文件：%1", "Files in batch: %1", "批次檔案：%1").arg(root.job.name || root.job.id || "") }
    Label { Layout.fillWidth: true; color: Theme.muted; textFormat: Text.PlainText; wrapMode: Text.WordWrap; text: root.job.id ? (root.job.created_at ? new Date(root.job.created_at).toLocaleString(Qt.locale()) + " · " : "") + "#" + root.batchKey(root.job.id) : "" }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text; text: Theme.state(root.job.state || "") + (root.job.cancellation_requested && root.active ? Theme.t(" · 取消中：等待当前可中断点", " · Cancelling: waiting for the current cancellation boundary", " · 取消中：等待目前可中斷點") : "") }
    Label { objectName: "imageGroupProgress"; visible: !!root.job.id; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("本次提交 · 已结束 %1 / %2 项", "This submission · %1 / %2 items finished", "本次提交 · 已結束 %1 / %2 項").arg(root.finished).arg(root.items.length) }
    ProgressBar {
        visible: !!root.job.id; Layout.fillWidth: true; from: 0; to: Math.max(1, root.items.length); value: root.finished
        background: Rectangle { implicitHeight: 4; radius: 2; color: Theme.field }
        contentItem: Item { implicitHeight: 4; Rectangle { width: parent.width * (root.finished / Math.max(1, root.items.length)); height: parent.height; radius: 2; color: Theme.accent } }
    }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("成功 %1 · 失败 %2 · 跳过 %3 · 取消 %4 · 中断 %5", "Success %1 · Failed %2 · Skipped %3 · Cancelled %4 · Interrupted %5", "成功 %1 · 失敗 %2 · 略過 %3 · 取消 %4 · 中斷 %5").arg(root.count("succeeded")).arg(root.count("failed")).arg(root.count("skipped")).arg(root.count("cancelled")).arg(root.count("interrupted")) }
    Flow {
        Layout.fillWidth: true; spacing: 6; visible: !!root.job.id
        Tool { objectName: "jobShowAll"; checkable: true; checked: !root.failuresOnly; text: Theme.t("全部 %1", "All %1", "全部 %1").arg(root.items.length); onClicked: root.failuresOnly = false }
        Tool { objectName: "jobShowFailures"; checkable: true; checked: root.failuresOnly; text: Theme.t("失败 / 中断 %1", "Failed / interrupted %1", "失敗 / 中斷 %1").arg(root.count("failed") + root.count("interrupted")); onClicked: root.failuresOnly = true }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 24
        Label { Layout.fillWidth: true; Layout.preferredWidth: 30; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("来源 / 选择", "Source / Selection", "來源 / 選擇") }
        Label { Layout.fillWidth: true; Layout.preferredWidth: 32; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("输出 / 规则", "Output / Rules", "輸出 / 規則") }
        Label { Layout.fillWidth: true; Layout.preferredWidth: 38; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("结果", "Result", "結果") }
        Item { Layout.preferredWidth: 70 }
    }
    ListView {
        objectName: "imageJobsTable"
        Layout.fillWidth: true; Layout.fillHeight: true; model: root.shownItems; clip: true; reuseItems: true; spacing: 6
        ScrollBar.vertical: ScrollBar {}
        delegate: ConversionItemRow {
            required property var modelData
            width: ListView.view.width; controller: root.controller; entry: modelData; preflight: false
        }
        Label { textFormat: Text.PlainText; anchors.centerIn: parent; visible: !root.job.id; color: Theme.muted; text: Theme.t("尚未提交转换任务", "No conversion job submitted", "尚未提交轉換工作") }
        Label { anchors.centerIn: parent; visible: !!root.job.id && root.failuresOnly && !root.shownItems.length; color: Theme.muted; text: Theme.t("暂无失败或中断项", "No failed or interrupted items", "暫無失敗或中斷項目") }
    }
    Flow {
        Layout.fillWidth: true; spacing: 6
        Tool { objectName: "pauseImageJob"; text: root.job.pause_requested || root.job.state === "paused" ? Theme.t("继续任务", "Resume", "繼續工作") : Theme.t("暂停任务", "Pause", "暫停工作"); visible: root.active; enabled: !root.service.busy && !root.job.cancellation_requested; hint: Theme.t("当前文件完成后暂停，继续时不重复已完成文件", "Pause after the current file; resume without repeating completed files", "目前檔案完成後暫停，繼續時不重複已完成檔案"); onClicked: root.service.pauseJob(root.job.id, !(root.job.pause_requested || root.job.state === "paused")) }
        Tool { objectName: "cancelImageJob"; text: root.job.cancellation_requested ? Theme.t("取消中", "Cancelling", "取消中") : Theme.t("取消任务", "Cancel job", "取消工作"); visible: root.active; enabled: !root.job.cancellation_requested; onClicked: root.service.cancel("job", root.job.id) }
        Tool { objectName: "retryImageJob"; text: Theme.t("继续未完成项…", "Continue unfinished items…", "繼續未完成項目…"); enabled: !!root.job.id && !root.service.busy && !root.active && root.items.some(i => !["succeeded", "skipped"].includes(i.status)); hint: Theme.t("重新检查未完成文件，通过预检后继续转换", "Recheck unfinished files, then continue conversion", "重新檢查未完成檔案，通過預檢後繼續轉換"); onClicked: root.retry(root.job.id) }
        Tool { objectName: "removeImageJob"; text: Theme.t("删除记录", "Delete record", "刪除記錄"); enabled: !!root.job.id && !root.active && !root.service.busy; hint: Theme.t("只删除此批次记录，保留导出文件；运行中的任务请先取消", "Remove this batch record, keeping exported files; cancel active jobs first", "只刪除此批次記錄，保留匯出檔案；執行中的工作請先取消"); onClicked: root.service.removeJob(root.job.id) }
        Tool { objectName: "revealJobOutput"; text: Theme.t("打开输出位置", "Show output location", "開啟輸出位置"); enabled: !!root.firstOutput; onClicked: root.controller.reveal(root.firstOutput) }
        Tool { text: Theme.t("复制任务报告", "Copy job report", "複製工作報告"); enabled: !!root.job.id; onClicked: root.controller.copy(JSON.stringify(root.job, null, 2)) }
    }
}
