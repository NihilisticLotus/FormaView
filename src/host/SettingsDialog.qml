pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
FocusScope {
    id: root
    objectName: "mcpSettingsDialog"
    required property var controller
    property string section: "general"
    readonly property bool dirty: general.dirty
    property var afterLeave: null
    visible: false
    signal opened()
    signal closed()
    function open(destination): void {
        if (!visible) general.reset()
        section = destination || "general"
        opened(); forceActiveFocus()
    }
    function finishClose(): void { const action = afterLeave; afterLeave = null; focus = false; closed(); if (action) action() }
    function leave(action): void { afterLeave = action; if (general.dirty) unsaved.open(); else finishClose() }
    function close(): void { leave(null) }
    onVisibleChanged: if (!visible) focus = false
    Keys.onEscapePressed: event => { close(); event.accepted = true }
    Rectangle { anchors.fill: parent; color: Theme.base }
    GeneralSettings {
        id: general; anchors.fill: parent; controller: root.controller; section: root.section
        visible: root.section !== "permissions"
        onSectionRequested: destination => root.section = destination
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 32; visible: root.section === "permissions"; spacing: 16
        Label { text: Theme.t("MCP 与权限", "MCP and permissions", "MCP 與權限"); color: Theme.text; font.pixelSize: 18 }
        Label { visible: !root.controller.imageJobs; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("当前构建未接入图片服务。", "Image service is not included in this build.", "目前組建未接入影像服務。") }
        Loader {
            Layout.fillWidth: true; Layout.fillHeight: true
            active: root.visible && root.section === "permissions" && !!root.controller.imageJobs
            onActiveChanged: if (active) setSource("McpSettings.qml", {controller: root.controller})
        }
    }
    Dialog {
        id: unsaved; objectName: "unsavedSettingsDialog"
        anchors.centerIn: Overlay.overlay; width: 440; modal: true; focus: true; padding: 24
        title: Theme.t("尚未保存的软件设置", "Unsaved app settings", "尚未儲存的軟體設定")
        font.pixelSize: Theme.formText
        background: Rectangle { radius: Theme.dialogRadius; color: Theme.raised; border.color: Theme.line }
        contentItem: Label { font.pixelSize: Theme.formText; text: Theme.t("离开将放弃本次设置更改。", "Leaving will discard these settings changes.", "離開將放棄本次設定變更。"); color: Theme.text; wrapMode: Text.WordWrap }
        footer: RowLayout {
            spacing: 12
            Item { Layout.fillWidth: true }
            Tool { objectName: "keepEditingSettings"; text: Theme.t("继续编辑", "Keep editing", "繼續編輯"); onClicked: unsaved.close() }
            Tool { objectName: "discardAndLeaveSettings"; text: Theme.t("放弃更改", "Discard changes", "放棄變更"); onClicked: { general.reset(); unsaved.close(); root.finishClose() } }
        }
    }
}
