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
    readonly property bool modelExport: request.options?.target === "fbx"
    readonly property string clipSummary: (result.clips || []).map(c => c.name + " · " + Number(c.start).toFixed(3) + "–" + Number(c.end).toFixed(3) + " s").join("\n")
    function filename(path): string { return String(path || "").replace(/\\/g, "/").split("/").pop() }
    onEntryChanged: expanded = false
    implicitHeight: content.implicitHeight + 24
    color: Theme.panel
    ColumnLayout {
        id: content; x: 12; y: 12; width: parent.width - 24; spacing: 10
        RowLayout {
            Layout.fillWidth: true; spacing: 24
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 30; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; textFormat: Text.PlainText; color: Theme.text; wrapMode: Text.WrapAnywhere; text: root.filename(root.request.input) }
                Label { Layout.fillWidth: true; font.pixelSize: Theme.compactText; color: Theme.muted; wrapMode: Text.WordWrap; textFormat: Text.PlainText; text: root.modelExport ? root.clipSummary || Theme.t("模型与绑定", "Model and bind pose", "模型與繫結") : Theme.t("子资源 ", "Subresource ", "子資源 ") + (root.request.options?.subresource ?? 0) }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 32; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; textFormat: Text.PlainText; color: Theme.text; wrapMode: Text.WrapAnywhere; text: root.filename(root.output) }
                Label { Layout.fillWidth: true; font.pixelSize: Theme.compactText; color: Theme.muted; wrapMode: Text.WordWrap; text: root.modelExport ? "FBX · " + (root.request.options?.include_mesh === false ? Theme.t("只导出动画", "Animation only", "只匯出動畫") : Theme.t("含模型", "Includes model", "含模型")) : (root.request.options?.target || "").toUpperCase() + " · " + (root.request.options?.output_scalar || Theme.t("保留源类型", "Preserve source type", "保留來源類型")) }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 38; Layout.alignment: Qt.AlignTop
                Label { Layout.fillWidth: true; color: root.entry.status === "failed" ? Theme.error : Theme.accent; wrapMode: Text.WordWrap; text: Theme.state(root.entry.status) + (root.result.output_exists ? Theme.t(" · 目标存在", " · Target exists", " · 目標已存在") : "") }
                ExplainedText { objectName: "conversionItemError"; Layout.fillWidth: true; kind: "error"; raw: root.entry.error ? root.entry.error.code + ": " + root.entry.error.message : "" }
                Repeater {
                    model: root.preflight ? (root.result.losses || []).concat(root.result.notices || []) : []
                    ExplainedText { required property var modelData; objectName: "conversionItemLoss"; Layout.fillWidth: true; kind: "loss"; code: modelData.code; detail: modelData.detail; tone: (root.result.notices || []).some(n => n.code === modelData.code) ? Theme.muted : Theme.accent }
                }
                Label { Layout.fillWidth: true; visible: !!root.entry.temporary_cleanup_error || !!root.entry.outcome_uncertain; color: Theme.error; wrapMode: Text.WordWrap; text: Theme.t("需检查中断或临时文件清理结果。", "Review interrupted output or temporary-file cleanup.", "需檢查中斷或暫存檔清理結果。") }
            }
            Tool { objectName: "conversionItemDetails"; Layout.alignment: Qt.AlignTop; quiet: true; text: root.expanded ? Theme.t("收起", "Less", "收起") : Theme.t("详情", "Details", "詳情"); onClicked: root.expanded = !root.expanded }
        }
        ColumnLayout {
            Layout.fillWidth: true; visible: root.expanded
            Readout { Layout.fillWidth: true; label: Theme.t("来源 → 确定输出", "Source → Resolved output", "來源 → 確定輸出"); value: (root.request.input || "") + "\n→ " + root.output }
            Readout { visible: root.modelExport; Layout.fillWidth: true; label: Theme.t("动作与根运动", "Clips and root motion", "動作與根運動"); value: root.clipSummary + "\n" + Theme.t("保留源根运动；每个动作独立 Stack / Take。", "Source root motion; each clip is an independent Stack/Take.", "保留來源根運動；每個動作獨立 Stack / Take。") }
            Readout { visible: root.modelExport; Layout.fillWidth: true; label: Theme.t("坐标与单位", "Axes and units", "座標與單位"); value: (root.result.output_coordinates?.preset || "—") + " · " + (root.result.output_coordinates?.unit || "—") + " · " + (root.result.output_coordinates?.metres_per_unit ?? "—") + " m/unit" }
            Readout { visible: root.modelExport && !!root.result.psa_companion; Layout.fillWidth: true; label: "PSA .config"; value: (root.result.psa_companion?.status || "—") + "\n" + (root.result.psa_companion?.candidate || "") + "\n" + JSON.stringify(root.result.psa_companion?.clip_rules || [], null, 2) }
            Tool { visible: !root.preflight && !!root.result.committed_output; objectName: "revealItemOutput"; text: Theme.t("打开输出位置", "Show output location", "開啟輸出位置"); onClicked: root.controller.reveal(root.result.committed_output) }
            Tool { text: Theme.t("复制完整详情", "Copy full details", "複製完整詳情"); onClicked: root.controller.copy(JSON.stringify(root.entry, null, 2)) }
        }
    }
}
