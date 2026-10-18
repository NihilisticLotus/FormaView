pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
    required property var controller
    required property var snapshot
    property alias acknowledged: acknowledge.checked
    readonly property string approvalDigest: snapshot.approval_digest || ""
    onApprovalDigestChanged: acknowledge.checked = false
    readonly property bool hasLoss: (snapshot.items || []).some(i => (i.preflight?.losses || []).length > 0)
    signal submit()
    signal back()
    spacing: 10
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.accent; wrapMode: Text.WordWrap; text: root.controller.imageJobs.busy ? Theme.t("正在固定输入与准备预检…", "Preparing fixed inputs and preflight…", "正在固定輸入與準備預檢…") : Theme.state(root.snapshot.state || "") + " · " + (root.snapshot.total || 0) }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WrapAnywhere; text: root.snapshot.id || Theme.t("修改参数后需重新预检。", "Changed parameters require a new preflight.", "修改參數後需重新預檢。") }
    ListView {
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; reuseItems: true; spacing: 6
        model: root.snapshot.items || []
        ScrollBar.vertical: ScrollBar {}
        delegate: Rectangle {
            id: row; required property var modelData
            width: ListView.view.width; implicitHeight: details.implicitHeight + 24; color: Theme.panel
            ColumnLayout {
                id: details; anchors.fill: parent; anchors.margins: 12; spacing: 6
                Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.text; wrapMode: Text.WrapAnywhere; text: row.modelData.request.input + "\n→ " + (row.modelData.preflight?.output || row.modelData.request.output) }
                Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: row.modelData.status === "failed" ? Theme.error : Theme.accent; wrapMode: Text.WordWrap; text: Theme.state(row.modelData.status) + (row.modelData.preflight?.output_exists ? Theme.t(" · 目标存在", " · Target exists", " · 目標已存在") : "") }
                Label { textFormat: Text.PlainText; Layout.fillWidth: true; visible: !!row.modelData.error; color: Theme.error; wrapMode: Text.WrapAnywhere; text: row.modelData.error ? row.modelData.error.code + ": " + row.modelData.error.message : "" }
                Repeater {
                    model: row.modelData.preflight?.losses || []
                    Label { textFormat: Text.PlainText; required property var modelData; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.accent; text: modelData.code + ": " + modelData.detail }
                }
                Tool { text: Theme.t("复制详情", "Copy details", "複製詳情"); onClicked: root.controller.copy(JSON.stringify(row.modelData, null, 2)) }
            }
        }
    }
    CheckBox { id: acknowledge; objectName: "acknowledgeImageLosses"; visible: root.hasLoss; enabled: root.snapshot.state === "ready"; text: Theme.t("已阅读并接受本计划列出的全部损失", "I reviewed and accept all losses listed for this plan", "已閱讀並接受本計畫列出的全部損失") }
    RowLayout {
        Tool { visible: root.controller.imageJobs.busy && !root.snapshot.id; text: Theme.t("取消准备", "Cancel preparation", "取消準備"); onClicked: root.controller.imageJobs.cancelPreparation() }
        Tool { text: Theme.t("返回修改", "Edit settings", "返回修改"); onClicked: root.back() }
        Tool { visible: root.snapshot.state === "planning"; enabled: !root.snapshot.cancellation_requested; text: root.snapshot.cancellation_requested ? Theme.t("取消中", "Cancelling", "取消中") : Theme.t("取消预检", "Cancel preflight", "取消預檢"); onClicked: root.controller.imageJobs.cancel("plan", root.snapshot.id) }
        Item { Layout.fillWidth: true }
        Tool { objectName: "submitImagePlan"; text: Theme.t("提交此计划", "Submit this plan", "提交此計畫"); enabled: root.snapshot.state === "ready" && !root.controller.imageJobs.busy && (!root.hasLoss || acknowledge.checked); onClicked: root.submit() }
    }
}
