pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Rectangle {
    id: root
    required property var controller
    required property var entry
    property bool preflight: false
    signal detailsRequested()
    readonly property var request: entry.request || ({})
    readonly property var result: preflight ? entry.preflight || ({}) : entry.result || entry.preflight || ({})
    readonly property string output: result.committed_output || result.output || request.output || ""
    readonly property bool modelExport: request.options?.target === "fbx"
    readonly property string clipSummary: (result.clips || []).map(c => c.name + " · " + Number(c.start).toFixed(3) + "–" + Number(c.end).toFixed(3) + " s").join("\n")
    function filename(path): string { return String(path || "").replace(/\\/g, "/").split("/").pop() }

    implicitHeight: content.implicitHeight + 12
    color: Theme.panel
    ColumnLayout {
        id: content; x: 8; y: 6; width: parent.width - 16; spacing: 4
        RowLayout {
            Layout.fillWidth: true; spacing: 12
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 30; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; textFormat: Text.PlainText; color: Theme.text; elide: Text.ElideMiddle; text: root.filename(root.request.input) }
                Label { Layout.fillWidth: true; font.pixelSize: Theme.compactText; color: Theme.muted; elide: Text.ElideRight; textFormat: Text.PlainText; text: root.modelExport ? root.clipSummary.replace(/\n/g, " · ") || Theme.t("模型与绑定", "Model and bind pose", "模型與繫結") : Theme.t("子资源 ", "Subresource ", "子資源 ") + (root.request.options?.subresource ?? 0) }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 32; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; textFormat: Text.PlainText; color: Theme.text; elide: Text.ElideMiddle; text: root.filename(root.output) }
                Label { Layout.fillWidth: true; font.pixelSize: Theme.compactText; color: Theme.muted; elide: Text.ElideRight; text: root.modelExport ? "FBX · " + (root.request.options?.include_mesh === false ? Theme.t("只导出动画", "Animation only", "只匯出動畫") : Theme.t("含模型", "Includes model", "含模型")) : (root.request.options?.target || "").toUpperCase() + " · " + (root.request.options?.output_scalar || Theme.t("保留源类型", "Preserve source type", "保留來源類型")) }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 38; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; color: root.entry.status === "failed" ? Theme.error : Theme.accent; elide: Text.ElideRight; text: Theme.state(root.entry.status) + (root.entry.error ? " · " + root.entry.error.code : "") + (root.result.output_exists ? Theme.t(" · 目标存在", " · Target exists", " · 目標已存在") : "") }
                ExplainedText { objectName: "conversionItemError"; Layout.fillWidth: true; kind: "error"; visible: root.preflight; raw: root.entry.error ? root.entry.error.code + ": " + root.entry.error.message : "" }
                Repeater {
                    model: root.preflight ? (root.result.losses || []).concat(root.result.notices || []) : []
                    ExplainedText { required property var modelData; objectName: "conversionItemLoss"; Layout.fillWidth: true; kind: "loss"; code: modelData.code; detail: modelData.detail; tone: (root.result.notices || []).some(n => n.code === modelData.code) ? Theme.muted : Theme.accent }
                }
                Label { Layout.fillWidth: true; visible: !!root.entry.temporary_cleanup_error || !!root.entry.outcome_uncertain; color: Theme.error; elide: Text.ElideRight; text: Theme.t("需检查中断或临时文件清理结果。", "Review interrupted output or temporary-file cleanup.", "需檢查中斷或暫存檔清理結果。") }
            }
            Tool {
                objectName: "conversionItemDetails"; Layout.alignment: Qt.AlignTop; quiet: true
                text: Theme.t("详情", "Details", "詳情")
                hint: (root.clipSummary || root.filename(root.request.input)) + "\n" + root.output
                Accessible.name: Theme.t("详情：%1", "Details: %1", "詳情：%1").arg(root.clipSummary || root.filename(root.request.input))
                onClicked: root.detailsRequested()
            }
        }
    }
}
