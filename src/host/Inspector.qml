pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
Rectangle {
    id: root
    required property var controller
    property string locatingId: ""
    readonly property int resourceCount: controller.resourceIssues.length
    onResourceCountChanged: if (resourcesSection) resourcesSection.checked = resourceCount > 0
    Component.onCompleted: resourcesSection.checked = resourceCount > 0
    objectName: "inspectorPane"
    color: Theme.panel; radius: Theme.groupRadius; border.color: Theme.line
    readonly property bool imageActive: !!controller.image && controller.image.available
    readonly property bool modelActive: !!controller.model && controller.model.available
    Loader {
        objectName: "modelInspectorLoader"
        anchors.fill: parent; anchors.margins: 12
        asynchronous: true
        active: root.modelActive; visible: active
        onActiveChanged: { if (active) setSource("ModelInspector.qml", {controller: root.controller}) }
        Component.onCompleted: { if (active) setSource("ModelInspector.qml", {controller: root.controller}) }
    }
    Loader {
        anchors.fill: parent; anchors.margins: 12
        active: root.imageActive; visible: active
        onActiveChanged: { if (active) setSource("ImageInspector.qml", {controller: root.controller}) }
        Component.onCompleted: { if (active) setSource("ImageInspector.qml", {controller: root.controller}) }
    }
    ColumnLayout {
        visible: !root.imageActive && !root.modelActive
        anchors.fill: parent; anchors.margins: 12; spacing: 14
        Label { text: Theme.t("检查", "Inspect", "檢查"); font.pixelSize: 15; font.bold: true; color: Theme.text }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        Label { Layout.fillWidth: true; visible: !root.controller.asset.path; text: Theme.t("选择文件以检查", "Select a file to inspect", "選取檔案以檢查"); color: Theme.muted; wrapMode: Text.WordWrap }
        Readout { Layout.fillWidth: true; visible: !!root.controller.asset.path; label: Theme.t("实际文件", "Current file", "目前檔案"); value: root.controller.asset.path || "" }
        Readout { Layout.fillWidth: true; visible: !!root.controller.asset.path; label: Theme.t("类型", "Type", "類型"); value: root.controller.asset.suffix || "—" }
        SectionHeader { id: statistics; Layout.fillWidth: true; text:Theme.t("文件信息 · 只读", "File details · Read only", "檔案資訊 · 唯讀"); checkable: true; enabled: !!root.controller.asset.path }
        ColumnLayout {
            visible: statistics.checked; Layout.fillWidth: true
            Readout { Layout.fillWidth: true; label: Theme.t("字节数", "Bytes", "位元組數"); value: String(root.controller.asset.bytes ?? "—") }
            Readout { Layout.fillWidth: true; label: Theme.t("修改时间", "Modified", "修改時間"); value: root.controller.asset.modified || "—" }
        }
        SectionHeader { id: resourcesSection; objectName: "genericResourceSection"; Layout.fillWidth: true; checkable: true; text:Theme.t("关联资源", "Linked resources", "關聯資源") + (root.controller.resourceIssues.length ? " · "+root.controller.resourceIssues.length : "") }
        Label { visible: resourcesSection.checked && root.controller.resourceIssues.length === 0; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("此文件类型没有可解析的关联资源。", "This file type has no linked resources to resolve.", "此檔案類型沒有可解析的關聯資源。") }
        ListView {
            objectName: "genericResourceList"
            Layout.fillWidth: true; Layout.preferredHeight: Math.min(contentHeight,320)
            visible: resourcesSection.checked && root.resourceCount > 0
            clip: true; reuseItems: true; model: root.controller.resourceIssues
            delegate: ColumnLayout {
                id: issue
                required property var modelData
                width: ListView.view.width; spacing: 6
                Readout { Layout.fillWidth: true; label: Theme.t("预期资源", "Expected resource", "預期資源"); value: issue.modelData.expected }
                ExplainedText { Layout.fillWidth: true; kind: "error"; raw: issue.modelData.detail }
                Readout { Layout.fillWidth: true; visible: issue.modelData.candidate.length>0; label: Theme.t("候选路径", "Candidate path", "候選路徑"); value: issue.modelData.candidate }
                Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: issue.modelData.state === "mapped" ? Theme.t("已记录路径，加载时仍须验证格式。", "Path recorded. Format validation is still required on load.", "已記錄路徑，載入時仍須驗證格式。") : Theme.t("候选检查仅验证路径与可读性。", "Candidate check verifies path and readability only.", "候選檢查僅驗證路徑與可讀性。") }
                RowLayout {
                    Tool { text: Theme.t("定位…", "Locate…", "定位…"); enabled: issue.modelData.state !== "checking"; onClicked: {root.locatingId=issue.modelData.id; candidate.open()} }
                    Tool { text: Theme.t("使用路径", "Use path", "使用路徑"); enabled: issue.modelData.state === "checked"; onClicked: root.controller.applyResource(issue.modelData.id) }
                }
            }
        }
        Item { Layout.fillHeight: true }
        Tool { Layout.fillWidth: true; enabled: !!root.controller.asset.path; text: Theme.t("复制文件路径", "Copy file path", "複製檔案路徑"); onClicked: root.controller.copy(root.controller.asset.path) }
    }
    FileDialog { id: candidate; title: Theme.t("定位资源", "Locate resource", "定位資源"); onAccepted: root.controller.validateResource(root.locatingId,selectedFile.toString()) }
}
