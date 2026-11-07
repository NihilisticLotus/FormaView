pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
AppToolWindow {
    id: root
    objectName: "quickExportWindow"
    required property string inputPath
    required property string preset
    // Geometry-cache frame export: the presented timeline sample (-1 otherwise).
    property int abcSample: -1
    property real abcSeconds: 0
    property string abcSourceSha256: ""
    readonly property var service: controller.imageJobs
    readonly property var inputs: [inputPath]
    property string phase: "settings"
    property string ownedPlan: ""
    property string previousPlan: ""
    property string ownedJob: ""
    property bool autoContinue: false
    property bool closingRequested: false
    property bool initialized: false
    readonly property bool settling: phase === "submitting"
    escapeClosesWindow: !jobs.detailsVisible && !results.detailsVisible
    title: preset === "texture" ? Theme.t("导出贴图", "Export texture", "匯出貼圖") : preset === "abcFrame" ? Theme.t("导出当前帧", "Export current frame", "匯出目前影格") : preset === "animation" ? Theme.t("导出动画", "Export animation", "匯出動畫") : Theme.t("导出模型", "Export model", "匯出模型")
    width: 900; height: preset === "animation" ? 780 : 660
    minimumWidth: 760; minimumHeight: 560
    visible: true
    function edit(): void {
        if (!visible) return
        autoContinue = false
        if (ownedPlan && service.plan.id === ownedPlan) service.invalidatePlan()
        ownedPlan = ""
        results.acknowledged = false
        phase = "settings"
    }
    function start(scope, options, nameTemplate, conflict, metadata): void {
        if (service.busy || phase !== "settings") return
        ownedPlan = ""; ownedJob = ""
        results.acknowledged = false
        previousPlan = service.plan.id || ""
        // A frame export always declares losses: preflight shows them here.
        if (preset !== "texture" && preset !== "abcFrame") {
            autoContinue = false
            phase = "direct"
            service.startExport(inputs, scope, options, nameTemplate, conflict, metadata)
            Qt.callLater(observe)
            return
        }
        autoContinue = true
        phase = "preflight"
        service.preflight(inputs, scope, options, nameTemplate, conflict, [], metadata)
        Qt.callLater(observe)
    }
    function submit(acknowledged): void {
        if (!visible || closingRequested || service.busy || !ownedPlan || service.plan.id !== ownedPlan || service.plan.state !== "ready") return
        autoContinue = false
        phase = "submitting"
        service.submit(acknowledged)
        Qt.callLater(observe)
    }
    function observe(): void {
        if (phase === "direct" && !service.busy && service.error && !ownedJob) {
            phase = "settings"
            return
        }
        if (phase === "submitting" && !service.busy && service.error) {
            phase = "preflight"
            if (closingRequested) close()
            return
        }
        if (!visible || closingRequested || phase !== "preflight" || service.busy) return
        if (!ownedPlan && service.plan.id && service.plan.id !== previousPlan) ownedPlan = service.plan.id
        if (service.error || (ownedPlan && service.plan.id !== ownedPlan)) { autoContinue = false; return }
        if (ownedPlan && service.plan.state === "ready" && autoContinue) {
            autoContinue = false
            // The initial Export click authorizes an ordinary, lossless export.
            // Any losses or skipped/failed items require review in this window.
            const items = service.plan.items || []
            if (items.length && items.every(i => i.status === "ready" && !(i.preflight?.losses || []).length)) submit(false)
        }
    }
    onClosing: event => {
        autoContinue = false
        closingRequested = true
        if (phase === "submitting") { event.accepted = false; return }
        if (phase === "preflight") { service.cancelPreparation(); service.invalidatePlan() }
        else if (ownedPlan && service.plan.id === ownedPlan) service.invalidatePlan()
    }
    Component.onCompleted: {
        if (!service.busy) service.dismissError()
        const path = inputPath.replace(/\\/g, "/")
        settings.outputDirectory = path.slice(0, path.lastIndexOf("/"))
        initialized = true
    }
    Connections {
        target: root.service
        function onChanged() { Qt.callLater(root.observe) }
        function onDirectExportFinished(jobId) {
            if (root.phase !== "direct") return
            root.ownedJob = jobId
            root.phase = jobId ? "job" : "settings"
        }
        function onSubmissionFinished(planId, jobId) {
            if (root.phase !== "submitting" || planId !== root.ownedPlan) return
            root.ownedJob = jobId
            root.phase = jobId ? "job" : "preflight"
            if (root.closingRequested) root.close()
        }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 20; spacing: 12
        anchors.topMargin: 20 + Math.max(0, root.header.mapToItem(parent, 0, root.header.height).y)
        Readout { objectName: "quickExportSource"; Layout.fillWidth: true; label: Theme.t("当前文件", "Current file", "目前檔案"); value: root.inputPath }
        Label { Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap
            text: root.phase === "settings" ? Theme.t("点击导出后逐项检查并导出，失败会列出原因。可继续操作预览窗口。", "Export checks and writes each item, with failure reasons shown here. You can keep using the preview.", "點擊匯出後逐項檢查並匯出，失敗會列出原因。可繼續操作預覽視窗。")
                : root.phase === "direct" ? Theme.t("正在准备导出。关闭窗口后仍在后台继续，可到任务中心查看进度或取消。", "Preparing export. Work continues after closing; view progress or cancel in the task center.", "正在準備匯出。關閉視窗後仍在背景繼續，可到工作中心查看進度或取消。")
                : Theme.t("进度与失败原因显示在此。已提交任务在关闭窗口后继续运行，可到任务中心查看。", "Progress and failures stay here. Submitted jobs continue after closing; find them in the task center.", "進度與失敗原因顯示於此。已提交工作在關閉視窗後繼續執行，可到工作中心查看。") }
        ExplainedText { objectName: "quickExportServiceError"; Layout.fillWidth: true; kind: "error"; raw: root.service.error; dismissible: true; onDismissed: root.service.dismissError() }
        Label { visible: root.closingRequested; text: Theme.t("提交完成后关闭…", "Closing after submission…", "提交完成後關閉…"); color: Theme.muted }
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: root.phase === "settings" ? 0 : root.phase === "job" || root.phase === "direct" ? 2 : 1
            ConversionSettings {
                id: settings; controller: root.controller; inputs: root.inputs
                quickMode: true; modelMode: root.preset !== "texture"; exportPreset: root.preset
                abcSample: root.abcSample; abcSeconds: root.abcSeconds; abcSourceSha256: root.abcSourceSha256
                onEdited: if (root.initialized) root.edit()
                onBack: root.close()
                onPreflight: (scope, options, nameTemplate, conflict, metadata) => root.start(scope, options, nameTemplate, conflict, metadata)
            }
            ConversionResults {
                id: results; controller: root.controller
                snapshot: root.ownedPlan && root.service.plan.id === root.ownedPlan ? root.service.plan : ({})
                enabled: !root.settling && !root.closingRequested
                onBack: { root.service.cancelPreparation(); root.edit() }
                onSubmit: root.submit(acknowledged)
                onCancelled: root.autoContinue = false
            }
            ImageJobsPane {
                id: jobs; objectName: "quickExportJob"; controller: root.controller; compact: true; fixedJobId: root.ownedJob
                awaitingJob: root.phase === "direct"
                onRetry: id => {
                    if (root.service.busy) return
                    root.previousPlan = root.service.plan.id || ""
                    root.ownedPlan = ""; root.autoContinue = false; results.acknowledged = false
                    root.phase = "preflight"; root.service.retry(id)
                }
            }
        }
        Tool { objectName: "quickExportEdit"; visible: root.phase === "job" && !jobs.active; enabled: !root.service.busy; text: Theme.t("修改设置再导出", "Change settings and export again", "修改設定再匯出"); onClicked: root.edit() }
    }
}
