pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
FocusScope {
    id: root
    objectName: "conversionWorkspace"
    required property var controller
    property var inputs: []
    property string selectionSnapshot: ""
    property bool initialized: false
    readonly property var service: controller.imageJobs
    signal closed()
    function open(page): void {
        const selected = controller.selectedImages()
        const snapshot = JSON.stringify(selected)
        if (!initialized || snapshot !== selectionSnapshot) {
            inputs = selected
            selectionSnapshot = snapshot
            settings.inputRoot = controller.directory
            if (page === 0) service.invalidatePlan()
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
                id: tabs; objectName: "conversionSteps"; Layout.preferredWidth: 360
                background: Rectangle { color: "transparent" }
                PageTab { text: Theme.t("1 设置", "1 Settings", "1 設定") }
                PageTab { text: Theme.t("2 预检", "2 Preflight", "2 預檢") }
                PageTab { text: Theme.t("3 任务", "3 Jobs", "3 工作") }
            }
            Item { Layout.fillWidth: true }
            Tool { objectName: "returnToPreview"; quiet: true; iconName: "return"; text: Theme.t("返回预览", "Back to preview", "返回預覽"); onClicked: root.close() }
        }
        Label { Layout.fillWidth: true; visible: !!root.service.error; wrapMode: Text.WrapAnywhere; color: Theme.error; text: root.service.error; textFormat: Text.PlainText }
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: tabs.currentIndex
            ConversionSettings {
                id: settings; controller: root.controller; inputs: root.inputs
                onChooseInputs: paths => root.inputs = paths
                onEdited: { root.service.invalidatePlan(); results.acknowledged = false }
                onBack: root.close()
                onPreflight: (scope, options, nameTemplate, conflict) => { root.service.preflight(root.inputs, scope, options, nameTemplate, conflict, settings.itemOverrides()); tabs.currentIndex = 1 }
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
