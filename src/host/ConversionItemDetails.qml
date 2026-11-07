pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
Popup {
    id: root
    objectName: "conversionDetails"
    required property var controller
    property var entry: ({})
    property bool preflight: false
    readonly property var request: entry.request || ({})
    readonly property var result: preflight ? entry.preflight || ({}) : entry.result || entry.preflight || ({})
    readonly property string output: result.committed_output || result.output || request.output || ""
    readonly property bool modelExport: request.options?.target === "fbx"
    readonly property string clipSummary: (result.clips || []).map(c => c.name + " · " + Number(c.start).toFixed(3) + "–" + Number(c.end).toFixed(3) + " s").join("\n")
    parent: Overlay.overlay
    x: parent ? Math.max(0, (parent.width - width) / 2) : 0
    y: parent ? Math.max(0, (parent.height - height) / 2) : 0
    width: parent ? Math.max(0, Math.min(680, parent.width - 24)) : 680
    height: parent ? Math.max(0, Math.min(520, parent.height - 24)) : 520
    modal: false; focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 16
    background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: Theme.radius }
    contentItem: ColumnLayout {
        RowLayout {
            Layout.fillWidth: true
            Label { Layout.fillWidth: true; text: Theme.t("导出详情", "Export details", "匯出詳情"); color: Theme.text; font.bold: true }
            Tool { objectName: "closeConversionDetails"; text: Theme.t("关闭", "Close", "關閉"); onClicked: root.close() }
        }
        ScrollView {
            id: detailScroll
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: detailScroll.availableWidth; spacing: 12
                Readout { Layout.fillWidth: true; label: Theme.t("来源 → 确定输出", "Source → Resolved output", "來源 → 確定輸出"); value: (root.request.input || "") + "\n→ " + root.output }
                Readout { Layout.fillWidth: true; label: Theme.t("状态", "Status", "狀態"); value: Theme.state(root.entry.status || "") }
                ExplainedText { objectName: "conversionItemError"; Layout.fillWidth: true; kind: "error"; raw: root.entry.error ? root.entry.error.code + ": " + root.entry.error.message : "" }
                Repeater {
                    model: (root.result.losses || []).concat(root.result.notices || [])
                    ExplainedText { required property var modelData; Layout.fillWidth: true; kind: "loss"; code: modelData.code; detail: modelData.detail }
                }
                Readout { visible: root.modelExport; Layout.fillWidth: true; label: Theme.t("动作与根运动", "Clips and root motion", "動作與根運動"); value: root.clipSummary + "\n" + Theme.t("保留源根运动；每个动作独立 Stack / Take。", "Source root motion; each clip is an independent Stack/Take.", "保留來源根運動；每個動作獨立 Stack / Take。") }
                Readout { visible: root.modelExport; Layout.fillWidth: true; label: Theme.t("坐标与单位", "Axes and units", "座標與單位"); value: (root.result.output_coordinates?.preset || "—") + " · " + (root.result.output_coordinates?.unit || "—") + " · " + (root.result.output_coordinates?.metres_per_unit ?? "—") + " m/unit" }
                Readout { visible: root.modelExport && !!root.result.psa_companion; Layout.fillWidth: true; label: "PSA .config"; value: JSON.stringify(root.result.psa_companion || {}, null, 2) }
                Tool { visible: !root.preflight && !!root.result.committed_output; objectName: "revealItemOutput"; text: Theme.t("打开输出位置", "Show output location", "開啟輸出位置"); onClicked: root.controller.reveal(root.result.committed_output) }
                Tool { text: Theme.t("复制完整详情", "Copy full details", "複製完整詳情"); onClicked: root.controller.copy(JSON.stringify(root.entry, null, 2)) }
            }
        }
    }
}
