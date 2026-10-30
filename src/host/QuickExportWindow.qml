pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
AppToolWindow {
    id: root
    objectName: "quickExportWindow"
    required property string inputPath
    required property string preset
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
    title: preset === "texture" ? Theme.t("导出贴图", "Export texture", "匯出貼圖") : preset === "animation" ? Theme.t("导出动画", "Export animation", "匯出動畫") : Theme.t("导出模型", "Export model", "匯出模型")
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
        if (phase === "job" && jobs.active) service.cancel("job", ownedJob)
        if (phase === "preflight") { service.cancelPreparation(); service.invalidatePlan() }
        else if (ownedPlan && service.plan.id === ownedPlan) service.invalidatePlan()
    }
    Component.onCompleted: {
        const path = inputPath.replace(/\\/g, "/")
        settings.outputDirectory = path.slice(0, path.lastIndexOf("/"))
        initialized = true
    }
    Connections {
        target: root.service
        function onChanged() { Qt.callLater(root.observe) }
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
            text: root.phase === "settings" ? Theme.t("点击导出后自动检查；需要确认时会在此提示。可继续操作预览窗口。", "Export checks automatically and asks here when needed. You can keep using the preview.", "點擊匯出後自動檢查；需要確認時會在此提示。可繼續操作預覽視窗。") : Theme.t("预检、确认与进度均在此窗口。关闭窗口会取消尚未完成的导出。", "Review and progress stay here. Closing cancels unfinished export.", "預檢、確認與進度均在此視窗。關閉視窗會取消尚未完成的匯出。") }
        ExplainedText { Layout.fillWidth: true; kind: "error"; raw: root.service.error }
        Label { visible: root.closingRequested; text: Theme.t("正在取消并关闭…", "Cancelling and closing…", "正在取消並關閉…"); color: Theme.muted }
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: root.phase === "settings" ? 0 : root.phase === "job" ? 2 : 1
            ConversionSettings {
                id: settings; controller: root.controller; inputs: root.inputs
                quickMode: true; modelMode: root.preset !== "texture"; exportPreset: root.preset
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
                onRetry: id => {
                    if (root.service.busy) return
                    root.previousPlan = root.service.plan.id || ""
                    root.ownedPlan = ""; root.autoContinue = false; results.acknowledged = false
                    root.phase = "preflight"; root.service.retry(id)
                }
            }
        }
        Tool { visible: root.phase === "job" && !jobs.active; enabled: !root.service.busy; text: Theme.t("修改设置再导出", "Change settings and export again", "修改設定再匯出"); onClicked: root.edit() }
    }
}
