pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
FocusScope {
    id: root
    objectName: "conversionWorkspace"
    required property var controller
    readonly property var imageInputs: controller.imageJobs.sourcePaths
    property var modelInputs: []
    property bool modelMode: false
    readonly property var inputs: modelMode ? modelInputs : imageInputs
    property string selectionSnapshot: ""
    property bool initialized: false
    readonly property var service: controller.imageJobs
    readonly property bool modelAvailable: !!service.capabilities.model_export
    signal closed()
    function open(page): void {
        const selected = controller.selectedImages()
        const models = controller.selectedModels()
        const snapshot = JSON.stringify([selected, models])
        if (!initialized || snapshot !== selectionSnapshot) {
            service.setSourceInputs(selected)
            modelInputs = models
            modelMode = modelAvailable && models.length > 0 && selected.length === 0
            selectionSnapshot = snapshot
            service.invalidatePlan()
        }
        initialized = true
        tabs.currentIndex = page
        visible = true
        forceActiveFocus()
    }
    function close(): void { focus = false; visible = false; closed() }
    onVisibleChanged: if (!visible) focus = false
    Keys.onEscapePressed: event => { close(); event.accepted = true }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: root.width < 1200 ? 24 : 40; spacing: 24
        RowLayout {
            Layout.fillWidth: true
            TabBar {
                id: tabs; objectName: "conversionSteps"; Layout.preferredWidth: 300
                background: Rectangle { color: "transparent" }
                PageTab { stepStyle: true; text: Theme.t("1 设置", "1 Settings", "1 設定") }
                PageTab { stepStyle: true; text: Theme.t("2 预检", "2 Preflight", "2 預檢") }
                PageTab { stepStyle: true; lastStep: true; text: Theme.t("3 任务", "3 Jobs", "3 工作") }
            }
            Item { Layout.fillWidth: true }
            Choice { objectName: "conversionCategory"; Layout.preferredWidth: 220; currentIndex: root.modelMode ? 1 : 0
                labels: root.modelAvailable ? [Theme.t("贴图转换", "Texture conversion", "貼圖轉換"), Theme.t("模型与动画导出", "Model and animation export", "模型與動畫匯出")] : [Theme.t("贴图转换", "Texture conversion", "貼圖轉換")]
                onActivated: { root.modelMode = currentIndex === 1; root.service.invalidatePlan(); results.acknowledged = false; tabs.currentIndex = 0 }
                Accessible.name: Theme.t("转换类别", "Conversion category", "轉換類別")
            }
            Tool { objectName: "returnToPreview"; quiet: true; iconName: "return"; text: Theme.t("返回预览", "Back to preview", "返回預覽"); onClicked: root.close() }
        }
        ExplainedText { objectName: "conversionServiceError"; Layout.fillWidth: true; kind: "error"; raw: root.service.error }
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: tabs.currentIndex
            ConversionSettings {
                id: settings; controller: root.controller; inputs: root.inputs; modelMode: root.modelMode
                onChooseInputs: paths => { if (root.modelMode) root.modelInputs = paths; else root.service.addSourceFiles(paths) }
                onEdited: { root.service.invalidatePlan(); results.acknowledged = false }
                onBack: root.close()
                onPreflight: (scope, options, nameTemplate, conflict, metadata) => { root.service.preflight(root.inputs, scope, options, nameTemplate, conflict, settings.itemOverrides(), metadata); tabs.currentIndex = 1 }
            }
            ConversionResults {
                id: results; controller: root.controller; snapshot: root.service.plan
                onBack: tabs.currentIndex = 0
                onSubmit: { root.service.submit(acknowledged); tabs.currentIndex = 2 }
            }
            ImageJobsPane { controller: root.controller; onRetry: id => { root.service.retry(id); tabs.currentIndex = 1; results.acknowledged = false } }
        }
    }
}
