pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ScrollView {
    id: root
    required property var controller
    readonly property var session: controller.image
    readonly property var info: session.info
    readonly property var data: info.dataWindow || ({})
    readonly property var displayWindow: info.displayWindow || ({})
    readonly property var channelNames: [""].concat(session.channels.map(c => c.name))
    readonly property var dimensions: session.dimensions
    readonly property var layers: info.layers || []
    function windowText(w): string { return "(" + w.x + ", " + w.y + ") · " + w.width + " × " + w.height }
    function resourceText(s): string {
        return "#" + s.index + " · " + s.name + " · " + s.width + "×" + s.height
            + (root.info.hasMips ? " / mip " + s.mip : "") + (root.info.hasArrays ? " / array " + s.array : "")
            + (root.info.hasFaces ? " / face " + s.face : "") + (root.info.hasSlices ? " / z " + s.slice : "")
            + (root.info.metadata["entry." + s.index + ".encoding"] ? " / " + root.info.metadata["entry." + s.index + ".encoding"] : "")
            + (root.info.metadata["entry." + s.index + ".directory_bits_per_pixel"] ? " / " + Theme.t("目录声明 ", "Header ", "目錄宣告 ") + root.info.metadata["entry." + s.index + ".directory_bits_per_pixel"] + " bpp" : "")
    }
    clip: true
    ColumnLayout {
        width: root.availableWidth; spacing: 10
        Readout { Layout.fillWidth: true; label: Theme.t("图片 · 只读", "Image · Read only", "影像 · 唯讀"); value: (root.info.format || "—") + " · " + root.data.width + " × " + root.data.height + "\n" + root.session.channels.map(c => c.name + " " + c.type).join(" / ") }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: root.info.block || ""; visible: !!text }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; visible: root.info.format === "GIF"; text: Theme.t("GIF：当前仅静态帧", "GIF: static frame only", "GIF：目前僅靜態影格") }
        ComboBox {
            objectName: "imageSubresource"; Layout.fillWidth: true; visible: root.session.subresources.length > 1 && root.info.format !== "DDS"
            model: root.session.subresources.map(s => root.resourceText(s))
            currentIndex: root.session.selected - root.session.subresourceOffset
            Accessible.name: Theme.t("子资源 / part / 图标条目", "Subresource / part / icon entry", "子資源 / part / 圖示項目")
            onActivated: root.session.selectSubresource(currentIndex + root.session.subresourceOffset)
        }
        RowLayout {
            Layout.fillWidth: true; visible: root.session.subresourceCount > 100 && root.info.format !== "DDS"
            Tool { text: "‹"; Accessible.name: Theme.t("上一页子资源", "Previous subresources", "上一頁子資源"); enabled: root.session.subresourceOffset > 0; onClicked: root.session.resourcePage(root.session.subresourceOffset - 100) }
            Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.muted; text: (root.session.subresourceOffset + 1) + " / " + root.session.subresourceCount }
            Tool { text: "›"; Accessible.name: Theme.t("下一页子资源", "Next subresources", "下一頁子資源"); enabled: root.session.subresourceOffset + 100 < root.session.subresourceCount; onClicked: root.session.resourcePage(root.session.subresourceOffset + 100) }
        }
        Repeater {
            model: ["mip", "array", "face", "slice"]
            RowLayout {
                id: dimensionRow
                required property string modelData
                readonly property var values: root.dimensions[modelData] || []
                Layout.fillWidth: true; visible: root.info.format === "DDS" && !!root.info[({mip: "hasMips", array: "hasArrays", face: "hasFaces", slice: "hasSlices"})[modelData]]
                Label { textFormat: Text.PlainText; color: Theme.text; text: dimensionRow.modelData === "slice" ? Theme.t("深度切片", "Depth slice", "深度切片") : dimensionRow.modelData === "array" ? Theme.t("数组", "Array", "陣列") : dimensionRow.modelData === "face" ? Theme.t("面", "Face", "面") : "Mip" }
                SpinBox {
                    objectName: "imageDimension-" + dimensionRow.modelData
                    Layout.fillWidth: true; editable: true; enabled: !root.session.busy && dimensionRow.values.length > 1
                    from: dimensionRow.values.length ? dimensionRow.values[0].value : 0
                    to: dimensionRow.values.length ? dimensionRow.values[dimensionRow.values.length - 1].value : 0
                    value: (root.dimensions.current || {})[dimensionRow.modelData] || 0
                    Accessible.name: dimensionRow.modelData
                    onValueModified: root.session.selectDimension(dimensionRow.modelData, value)
                }
                Label { textFormat: Text.PlainText; visible: dimensionRow.modelData === "face"; color: Theme.muted; text: ["+X", "−X", "+Y", "−Y", "+Z", "−Z"][(root.dimensions.current || {}).face || 0] }
            }
        }
        ComboBox {
            objectName: "imageLayer"; Layout.fillWidth: true; visible: root.info.format === "EXR" && root.layers.length > 0
            model: root.layers.map(v => v || Theme.t("基础层（无前缀）", "Base layer (no prefix)", "基礎層（無前綴）"))
            currentIndex: {
                const names = (root.info.mapping || []).filter(v => !!v)
                const prefixes = names.map(v => v.includes(".") ? v.slice(0, v.lastIndexOf(".")) : "")
                return prefixes.length && prefixes.every(v => v === prefixes[0]) ? root.layers.indexOf(prefixes[0]) : -1
            }
            Accessible.name: Theme.t("实际通道层", "Source channel layer", "實際通道層")
            onActivated: root.session.selectLayer(root.layers[currentIndex])
        }
        Tool { id: channelSection; Layout.fillWidth: true; checkable: true; text: (checked ? "▾ " : "▸ ") + Theme.t("通道与解释", "Channels and interpretation", "通道與解釋") }
        ColumnLayout {
            visible: channelSection.checked; Layout.fillWidth: true
            Repeater {
                model: ["R", "G", "B", "A"]
                RowLayout {
                    required property string modelData
                    required property int index
                    id: mappingRow; Layout.fillWidth: true
                    Label { textFormat: Text.PlainText; text: mappingRow.modelData; color: Theme.text }
                    ComboBox {
                        Layout.fillWidth: true; model: root.channelNames
                        currentIndex: root.channelNames.indexOf((root.info.mapping || [])[mappingRow.index] || "")
                        Accessible.name: mappingRow.modelData + Theme.t(" 映射", " mapping", " 映射")
                        onActivated: { const names = root.info.mapping.slice(); names[mappingRow.index] = currentText; root.session.mapChannels(names) }
                    }
                }
            }
            ComboBox {
                Layout.fillWidth: true; model: [Theme.t("数据：跳过颜色变换", "Data: bypass colour", "資料：略過色彩變換"), Theme.t("颜色：使用显示变换", "Colour: display transform", "色彩：使用顯示變換")]
                currentIndex: root.session.display.colour ? 1 : 0; onActivated: root.session.setting("colour", currentIndex === 1)
                Accessible.name: Theme.t("查看解释", "Viewing interpretation", "檢視解釋")
            }
            ComboBox {
                Layout.fillWidth: true; model: [Theme.t("Alpha 未知", "Alpha unknown", "Alpha 未知"), Theme.t("Alpha 缺失", "Alpha absent", "Alpha 缺失"), Theme.t("Alpha 直通", "Alpha straight", "Alpha 直通"), Theme.t("Alpha 预乘", "Alpha premultiplied", "Alpha 預乘")]
                currentIndex: root.session.display.alpha; enabled: !!(root.info.mapping || [])[3]
                onActivated: root.session.setting("alpha", currentIndex)
                Accessible.name: Theme.t("显示 Alpha 解释", "Display alpha interpretation", "顯示 Alpha 解釋")
            }
            Label { textFormat: Text.PlainText; color: Theme.text; text: Theme.t("曝光 EV · 仅显示", "Exposure EV · Display only", "曝光 EV · 僅顯示") }
            ExposureSpinBox { Layout.fillWidth: true; value: Math.round(root.session.display.exposure * 100); enabled: root.session.display.colour && root.session.display.channel < 0; onValueModified: root.session.setting("exposure", realValue); Accessible.name: Theme.t("曝光 EV", "Exposure EV", "曝光 EV") }
            CheckBox { text: Theme.t("线性 → sRGB · 仅显示", "Linear → sRGB · Display only", "線性 → sRGB · 僅顯示"); checked: root.session.display.srgb; enabled: root.session.display.colour && root.session.display.channel < 0; onToggled: root.session.setting("srgb", checked) }
            Readout { Layout.fillWidth: true; label: Theme.t("源颜色标签", "Source colour tag", "來源色彩標籤"); value: root.info.colourSpace || Theme.t("未知", "Unknown", "未知") }
        }
        Tool { id: metadataSection; Layout.fillWidth: true; checkable: true; text: (checked ? "▾ " : "▸ ") + Theme.t("元数据与边界", "Metadata and windows", "中繼資料與邊界") }
        ColumnLayout {
            visible: metadataSection.checked; Layout.fillWidth: true
            Readout { Layout.fillWidth: true; label: "dataWindow"; value: root.windowText(root.data) }
            Readout { Layout.fillWidth: true; label: "displayWindow"; value: root.windowText(root.displayWindow) }
            Readout { Layout.fillWidth: true; label: "SHA-256"; value: root.info.sha256 || "—" }
            Repeater { model: Object.keys(root.info.metadata || {}); Readout { required property string modelData; Layout.fillWidth: true; label: modelData; value: String(root.info.metadata[modelData]) } }
        }
    }
}
