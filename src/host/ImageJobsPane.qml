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
    readonly property var preparation: service.preparation || ({})
    readonly property bool preparing: !!preparation.active && (awaitingJob || !compact)
    readonly property string preparationStage: {
        if (preparation.cancellation_requested) return Theme.t("取消中：等待当前可中断点", "Cancelling: waiting for the current cancellation boundary", "取消中：等待目前可中斷點")
        switch (preparation.stage) {
        case "queued": return Theme.t("等待准备工作开始", "Waiting to start preparation", "等待準備工作開始")
        case "setup": return Theme.t("初始化导出服务", "Initializing export services", "初始化匯出服務")
        case "inspect": return Theme.t("读取源文件与片段目录", "Reading source and clip inventory", "讀取來源檔案與片段目錄")
        case "expand": return Theme.t("生成导出条目", "Expanding export items", "產生匯出項目")
        case "validate": return Theme.t("检查导出设置与文件名", "Checking export settings and names", "檢查匯出設定與檔名")
        case "record": return Theme.t("保存任务条目", "Saving task items", "儲存工作項目")
        case "protect": return Theme.t("校验源文件与依赖保护", "Verifying source and dependency protection", "校驗來源檔案與相依保護")
        case "publish": return Theme.t("提交导出任务", "Submitting export job", "提交匯出工作")
        default: return Theme.t("准备导出", "Preparing export", "準備匯出")
        }
    }
    property bool failuresOnly: false
    property string detailJobId: ""
    property int detailIndex: -1
    readonly property bool detailsVisible: details.visible
    function openDetails(entry): void {
        detailJobId = job.id
        detailIndex = entry.index
        details.entry = entry
        details.open()
    }
    function refreshDetails(): void {
        if (!details.visible) return
        const snapshot = service.jobSnapshot(detailJobId)
        if (!snapshot.id && service.busy) return
        const item = (snapshot.items || []).find(i => i.index === detailIndex)
        if (item) details.entry = item
        else details.close()
    }
    ConversionItemDetails { id: details; controller: root.controller }
    Timer { interval: 200; repeat: true; running: details.visible; onTriggered: root.refreshDetails() }
    ColumnLayout {
        visible: root.preparing; Layout.fillWidth: true
        Label { objectName: "exportPreparationStage"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text; text: root.preparationStage }
        Label { Layout.fillWidth: true; visible: !!root.preparation.input; elide: Text.ElideMiddle; textFormat: Text.PlainText; color: Theme.muted; text: root.preparation.input || "" }
        ProgressBar {
            id: preparationBar; objectName: "exportPreparationProgress"; Layout.fillWidth: true
            indeterminate: !(root.preparation.total > 0)
            from: 0; to: Math.max(1, root.preparation.total || 0); value: root.preparation.completed || 0
            Accessible.name: root.preparationStage
            background: Rectangle { implicitHeight: 6; radius: 3; color: Theme.field }
            contentItem: Item {
                implicitHeight: 6; clip: true
                Rectangle { visible: !preparationBar.indeterminate; width: parent.width * preparationBar.position; height: parent.height; radius: 3; color: Theme.accent }
                Rectangle {
                    id: preparationPulse; visible: preparationBar.indeterminate
                    width: parent.width * 0.2; height: parent.height; radius: 3; color: Theme.accent
                    XAnimator on x {
                        running: preparationBar.visible && preparationBar.indeterminate
                        from: -preparationPulse.width; to: preparationBar.availableWidth
                        duration: 1400; loops: Animation.Infinite
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                objectName: "exportPreparationElapsed"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted
                text: Theme.t("已用 %1 秒", "Elapsed %1 s", "已用 %1 秒").arg(Math.floor((root.preparation.elapsed_ms || 0) / 1000))
                    + (root.preparation.total > 0 ? Theme.t(" · 当前步骤 %1 / %2 项", " · Current step %1 / %2 items", " · 目前步驟 %1 / %2 項").arg(root.preparation.completed || 0).arg(root.preparation.total)
                       : Theme.t(" · 当前步骤无法估算百分比", " · Percentage unavailable for this step", " · 目前步驟無法估算百分比"))
            }
            Tool {
                objectName: "cancelExportPreparation"; enabled: !root.preparation.cancellation_requested
                text: root.preparation.cancellation_requested ? Theme.t("取消中…", "Cancelling…", "取消中…") : Theme.t("取消准备", "Cancel preparation", "取消準備")
                onClicked: root.service.cancelPreparation()
            }
        }
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
    readonly property var shownItems: items.filter(i => {
        if (failuresOnly && i.status !== "failed" && i.status !== "interrupted") return false
        const keyword = itemSearch.text.trim().toLocaleLowerCase()
        const result = i.result || i.preflight || {}
        return !keyword || [i.request?.input, i.request?.output, result.committed_output,
            ...(result.clips || []).map(c => c.name)].join("\n").toLocaleLowerCase().includes(keyword)
    })
    readonly property var categories: Array.from(new Set(service.jobs.map(j => j.category || ""))).filter(c => !!c).sort()
    property string selectedCategory: ""
    readonly property var visibleJobs: service.jobs.filter(j => !selectedCategory || j.category === selectedCategory).slice().sort((a, b) => String(b.created_at || "").localeCompare(String(a.created_at || "")))
    function categoryLabel(value): string { return value === "model" ? Theme.t("模型与动画", "Models and animation", "模型與動畫") : value === "image" ? Theme.t("图片", "Images", "影像") : value }
    function batchKey(value): string { return String(value || "").replace(/-job$/, "").slice(-8) }
    readonly property bool active: ["queued", "running", "paused", "pausing"].includes(job.state)
    readonly property bool working: ["queued", "running", "pausing"].includes(job.state)
    property double activityClock: Date.now()
    onJobChanged: activityClock = Date.now()
    Timer {
        interval: 1000; repeat: true
        running: root.working && root.visible && !!root.Window.window?.visible
        onTriggered: root.activityClock = Date.now()
    }
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
    ExplainedText { objectName: "jobServiceError"; Layout.fillWidth: true; visible: !root.compact && !!root.service.error; kind: "error"; raw: root.service.error; dismissible: true; onDismissed: root.service.dismissError() }
    ExplainedText {
        objectName: "imageJobError"
        Layout.fillWidth: true; kind: "error"
        raw: root.job.error ? (root.job.error.code || "error") + ": " + (root.job.error.message || "") : ""
    }
    Connections { target: root.service; function onChanged() { Qt.callLater(root.syncJobSelection); Qt.callLater(root.refreshDetails) } }
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
    RowLayout {
        visible: !!root.job.id && root.working; Layout.fillWidth: true
        BusyIndicator {
            objectName: "exportJobActivity"; Layout.preferredWidth: 20; Layout.preferredHeight: 20
            padding: 1; palette.dark: Theme.accent
            running: root.working && root.visible && !!root.Window.window?.visible
            Accessible.name: Theme.t("正在处理当前文件", "Processing the current file", "正在處理目前檔案")
        }
        Label {
            objectName: "exportJobElapsed"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted
            text: (root.job.cancellation_requested
                ? Theme.t("正在取消，等待当前可中断点", "Cancelling, waiting for the current cancellation boundary", "正在取消，等待目前可中斷點")
                : Theme.t("正在检查或导出当前文件", "Checking or exporting the current file", "正在檢查或匯出目前檔案"))
                + Theme.t(" · 自提交起 %1 秒（含等待和暂停）", " · %1 s since submission (including waits and pauses)", " · 自提交起 %1 秒（含等待和暫停）")
                    .arg(Math.floor(Math.max(0, root.activityClock - (Date.parse(root.job.created_at || "") || root.activityClock)) / 1000))
        }
    }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("成功 %1 · 失败 %2 · 跳过 %3 · 取消 %4 · 中断 %5", "Success %1 · Failed %2 · Skipped %3 · Cancelled %4 · Interrupted %5", "成功 %1 · 失敗 %2 · 略過 %3 · 取消 %4 · 中斷 %5").arg(root.count("succeeded")).arg(root.count("failed")).arg(root.count("skipped")).arg(root.count("cancelled")).arg(root.count("interrupted")) }
    Flow {
        Layout.fillWidth: true; spacing: 6; visible: !!root.job.id
        Tool { objectName: "jobShowAll"; checkable: true; checked: !root.failuresOnly; text: Theme.t("全部 %1", "All %1", "全部 %1").arg(root.items.length); onClicked: root.failuresOnly = false }
        Tool { objectName: "jobShowFailures"; checkable: true; checked: root.failuresOnly; text: Theme.t("失败 / 中断 %1", "Failed / interrupted %1", "失敗 / 中斷 %1").arg(root.count("failed") + root.count("interrupted")); onClicked: root.failuresOnly = true }
    }
    Field {
        id: itemSearch; objectName: "exportItemSearch"; Layout.fillWidth: true
        placeholderText: Theme.t("查找片段名、来源或输出", "Find clip, source or output", "尋找片段名、來源或輸出")
        Accessible.name: placeholderText
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
            onDetailsRequested: root.openDetails(modelData)
        }
        Label { objectName: "noConversionJob"; textFormat: Text.PlainText; anchors.centerIn: parent; visible: !root.job.id && !root.preparing; color: Theme.muted; text: Theme.t("尚未提交转换任务", "No conversion job submitted", "尚未提交轉換工作") }
        Label { anchors.centerIn: parent; visible: !!root.job.id && !root.shownItems.length; color: Theme.muted; text: Theme.t("没有符合筛选条件的条目", "No items match the filters", "沒有符合篩選條件的項目") }
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
