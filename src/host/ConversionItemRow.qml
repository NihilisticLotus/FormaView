pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Rectangle {
    id: root
    required property var controller
    required property var entry
    property bool preflight: false
    property bool expanded: false
    readonly property var request: entry.request || ({})
    readonly property var result: preflight ? entry.preflight || ({}) : entry.result || ({})
    readonly property string output: result.committed_output || result.output || request.output || ""
    function filename(path): string { return String(path || "").replace(/\\/g, "/").split("/").pop() }
    onEntryChanged: expanded = false
    implicitHeight: content.implicitHeight + 24
    color: Theme.panel
    ColumnLayout {
        id: content; anchors.fill: parent; anchors.margins: 12; spacing: 10
        RowLayout {
            Layout.fillWidth: true; spacing: 24
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 30; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; textFormat: Text.PlainText; color: Theme.text; wrapMode: Text.WrapAnywhere; text: root.filename(root.request.input) }
                Label { Layout.fillWidth: true; font.pixelSize: Theme.compactText; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("子资源 ", "Subresource ", "子資源 ") + (root.request.options?.subresource ?? 0) }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 32; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; textFormat: Text.PlainText; color: Theme.text; wrapMode: Text.WrapAnywhere; text: root.filename(root.output) }
                Label { Layout.fillWidth: true; font.pixelSize: Theme.compactText; color: Theme.muted; wrapMode: Text.WordWrap; text: (root.request.options?.target || "").toUpperCase() + " · " + (root.request.options?.output_scalar || Theme.t("保留源类型", "Preserve source type", "保留來源類型")) }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 38; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; color: root.entry.status === "failed" ? Theme.error : Theme.accent; wrapMode: Text.WordWrap; text: Theme.state(root.entry.status) + (root.result.output_exists ? Theme.t(" · 目标存在", " · Target exists", " · 目標已存在") : "") }
                Label { Layout.fillWidth: true; textFormat: Text.PlainText; visible: !!root.entry.error; color: Theme.error; wrapMode: Text.WrapAnywhere; text: root.entry.error ? root.entry.error.code + ": " + root.entry.error.message : "" }
                Repeater {
                    model: root.preflight ? root.result.losses || [] : []
                    Label { required property var modelData; Layout.fillWidth: true; font.pixelSize: Theme.compactText; textFormat: Text.PlainText; color: Theme.accent; wrapMode: Text.WrapAnywhere; text: modelData.code + ": " + modelData.detail }
                }
                Label { Layout.fillWidth: true; visible: !!root.entry.temporary_cleanup_error || !!root.entry.outcome_uncertain; color: Theme.error; wrapMode: Text.WordWrap; text: Theme.t("需检查中断或临时文件清理结果。", "Review interrupted output or temporary-file cleanup.", "需檢查中斷或暫存檔清理結果。") }
            }
            Tool { Layout.alignment: Qt.AlignTop; quiet: true; text: root.expanded ? Theme.t("收起", "Less", "收起") : Theme.t("详情", "Details", "詳情"); onClicked: root.expanded = !root.expanded }
        }
        ColumnLayout {
            Layout.fillWidth: true; visible: root.expanded
            Readout { Layout.fillWidth: true; label: Theme.t("来源 → 确定输出", "Source → Resolved output", "來源 → 確定輸出"); value: (root.request.input || "") + "\n→ " + root.output }
            Tool { text: Theme.t("复制完整详情", "Copy full details", "複製完整詳情"); onClicked: root.controller.copy(JSON.stringify(root.entry, null, 2)) }
        }
    }
}
