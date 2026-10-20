pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
    objectName: "pixelPanel"
    required property var controller
    readonly property var sample: controller.image.pixel
    spacing: 6
    ScrollView {
        id: readouts
        objectName: "pixelReadouts"
        Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0
        implicitHeight: content.implicitHeight + topPadding + bottomPadding
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        clip: true
        ColumnLayout {
            id: content; width: readouts.availableWidth; spacing: 6
        Label { objectName: "pixelSourceDescription"; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: root.sample.valid ? (root.controller.image.info.block ? Theme.t("BC 有损解压值", "BC lossy decoded values", "BC 有損解壓值") : Theme.t("解码原值", "Decoded source values", "解碼原值")) + " · (" + root.sample.x + ", " + root.sample.y + ") · tile (" + root.sample.tileX + ", " + root.sample.tileY + ")" : Theme.t("移入贴图取样 · 离开保留 · 单击固定", "Hover to sample · Leave to retain · Click to pin", "移入貼圖取樣 · 離開保留 · 單擊固定") }
        RowLayout {
            Layout.fillWidth: true; spacing: 16
            Readout { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.alignment: Qt.AlignTop; label: Theme.t("源值 → 解释值", "Source → Interpreted", "來源值 → 解釋值"); value: root.sample.valid ? root.sample.source.map(c => c.name + " · " + c.type + ": " + c.source + " → " + c.interpreted).join("\n") : root.controller.image.channels.map(c => c.name + " · " + c.type + ": —").join("\n") || "—" }
            Readout { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.alignment: Qt.AlignTop; label: Theme.t("合成前显示值 · 未裁切", "Pre-composite display · Unclipped", "合成前顯示值 · 未裁切"); value: ["R", "G", "B", "A"].map((name, i) => name + ": " + (root.sample.valid ? root.sample.display[i] : "—")).join("\n") }
        }
        }
    }
    Tool {
        objectName: "copyPixelValues"
        text: Theme.t("复制全部读数", "Copy all values", "複製全部讀數")
        enabled: root.sample.valid === true
        onClicked: root.controller.copy(root.controller.image.pixelReport())
    }
}
