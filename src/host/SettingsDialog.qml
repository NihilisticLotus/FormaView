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
    signal addressRequested()
    function open(destination): void {
        if (!visible) general.reset()
        section = destination || "general"
        opened(); panel.forceActiveFocus()
    }
    function finishClose(): void { const action = afterLeave; afterLeave = null; focus = false; closed(); if (action) action() }
    function leave(action): void { afterLeave = action; if (general.dirty) unsaved.open(); else finishClose() }
    function close(): void { leave(null) }
    onVisibleChanged: if (!visible) focus = false
    Keys.onEscapePressed: event => { close(); event.accepted = true }
    Popup {
        id: panel; objectName: "settingsModal"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(860, parent.width - 32)
        height: Math.min(760, parent.height - 52)
        modal: true; focus: true; padding: 0
        visible: root.visible
        popupType: Popup.Item
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.reduceMotion ? 0 : 100 } }
        closePolicy: Popup.NoAutoClose
        Overlay.modal: Rectangle { color: Theme.light ? "#57404b56" : "#740a1019" }
        background: GlassPanel { color: Theme.light ? "#e0e3eaf0" : "#ef273847" }
        Shortcut {
            sequence: KeyBindings.binding("address"); enabled: panel.opened && !unsaved.visible
                && !(root.Window.window?.activeFocusItem instanceof TextInput)
                && !(root.Window.window?.activeFocusItem instanceof TextEdit)
            onActivated: root.leave(() => root.addressRequested())
        }
        contentItem: FocusScope {
            layer.enabled: unsaved.visible || (root.Window.window?.frostDepth ?? 0) > 0
            layer.effect: FrostEffect {}
            focus: true
            Keys.onEscapePressed: event => { root.close(); event.accepted = true }
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 59; Layout.leftMargin: 22; Layout.rightMargin: 16
                    Label { Layout.fillWidth: true; color: Theme.text; font.pixelSize: 19; text: Theme.t("软件设置","Settings","軟體設定") }
                    Tool { objectName: "closeSettings"; quiet: true; iconName: "close"; hint: Theme.t("关闭设置","Close settings","關閉設定"); onClicked: root.close() }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.glassEdge }
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                    NavigationRail {
                        objectName: "navigationRail"; page: "settings"; section: root.section
                        Layout.preferredWidth: panel.width < 740 ? 150 : 185; Layout.fillHeight: true
                        onNavigate: destination => root.section = destination
                    }
                    Item {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        GeneralSettings {
                            id: general; anchors.fill: parent; controller: root.controller; section: root.section
                            visible: root.section !== "permissions"
                            onSectionRequested: destination => root.section = destination
                        }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 20; visible: root.section === "permissions"; spacing: 16
        Label { text: Theme.t("MCP 与权限", "MCP and permissions", "MCP 與權限"); color: Theme.text; font.pixelSize: 18 }
        Label { visible: !root.controller.imageJobs; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("当前构建未接入图片服务。", "Image service is not included in this build.", "目前組建未接入影像服務。") }
        Loader {
            Layout.fillWidth: true; Layout.fillHeight: true
            active: root.visible && root.section === "permissions" && !!root.controller.imageJobs
            onActiveChanged: if (active) setSource("McpSettings.qml", {controller: root.controller})
        }
    }
                    }
                }
            }
        }
    }
    FrostDialog {
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
