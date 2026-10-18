pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
ColumnLayout {
    id: root
    required property var controller
    required property var inputs
    property alias inputRoot: input.text
    function itemOverrides(): var { return perItem.options() }
    signal edited()
    signal chooseInputs(var paths)
    signal preflight(var scope, var options, string nameTemplate, string conflict)
    readonly property var targets: controller.imageJobs.capabilities.write_targets
    function options(): var {
        const o = {target: targets[target.currentIndex], subresource: resource.value,
            alpha_action: ["preserve", "drop", "composite"][alphaAction.currentIndex],
            meaning: colour.checked ? "colour" : "data", transfer: ["preserve", "linear_to_srgb", "srgb_to_linear"][transfer.currentIndex],
            exposure: exposure.realValue, background_linear: [background.value / 100, background.value / 100, background.value / 100],
            quantization: quantize.checked ? "clamp_round" : "reject_loss", non_finite: replaceNonFinite.checked ? "replace_zero" : ([1, 2].includes(depth.currentIndex) ? "reject" : "preserve"),
            window: flatten.checked ? "selected_data" : "preserve", metadata: dropMetadata.checked ? "drop" : "preserve_supported",
            allow_lossy: allowLossy.checked, quality: quality.value, png_compression: pngCompression.value,
            webp_lossless: webpLossless.checked, exr_tiled: exrTiled.checked,
            tiff_compression: ["none", "lzw", "deflate", "packbits"][tiffCompression.currentIndex], dds_format: ddsFormat.currentValue || 0}
        if (depth.currentIndex > 0) o.output_scalar = ["", "uint8", "uint16", "half", "float32"][depth.currentIndex]
        if (sourceAlpha.currentIndex > 0) o.source_alpha = ["", "absent", "straight", "premultiplied"][sourceAlpha.currentIndex]
        return o
    }
    ScrollView {
    id: settingsScroll
    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
    ColumnLayout {
        width: settingsScroll.availableWidth; spacing: 10
        Readout { Layout.fillWidth: true; label: Theme.t("固定输入列表 · 最多 64 项", "Fixed input list · Up to 64 items", "固定輸入清單 · 最多 64 項"); value: root.inputs.join("\n") || "—" }
        Label { color: Theme.text; text: Theme.t("只读输入范围", "Read-only input scope", "唯讀輸入範圍") }
        RowLayout {
            Layout.fillWidth: true
            Field { id: input; objectName: "conversionInputRoot"; Layout.fillWidth: true; onTextEdited: root.edited(); Accessible.name: Theme.t("只读输入目录", "Read-only input directory", "唯讀輸入目錄") }
            Tool { text: Theme.t("选择…", "Choose…", "選擇…"); onClicked: inputFolder.open() }
        }
        Label { color: Theme.text; text: Theme.t("输出目录与写入范围", "Output directory and write scope", "輸出目錄與寫入範圍") }
        RowLayout {
            Layout.fillWidth: true
            Field { id: output; objectName: "conversionOutputRoot"; Layout.fillWidth: true; onTextEdited: root.edited(); Accessible.name: Theme.t("输出目录", "Output directory", "輸出目錄") }
            Tool { text: Theme.t("选择…", "Choose…", "選擇…"); onClicked: outputFolder.open() }
        }
        Label { color: Theme.text; text: Theme.t("名称模板 · {name} / {index} / {ext}", "Filename template · {name} / {index} / {ext}", "名稱範本 · {name} / {index} / {ext}") }
        Field { id: naming; Layout.fillWidth: true; text: "{name}-converted.{ext}"; onTextEdited: root.edited(); Accessible.name: Theme.t("名称模板", "Filename template", "名稱範本") }
        GridLayout {
            Layout.fillWidth: true; columns: width > 700 ? 4 : 2; columnSpacing: 12; rowSpacing: 8
            Label { text: Theme.t("目标", "Target", "目標"); color: Theme.text }
            ComboBox { id: target; objectName: "conversionTarget"; Layout.fillWidth: true; model: root.targets.map(t => t.toUpperCase()); onActivated: root.edited(); Accessible.name: Theme.t("目标格式", "Target format", "目標格式") }
            Label { text: Theme.t("位深", "Precision", "精度"); color: Theme.text }
            ComboBox { id: depth; objectName: "conversionDepth"; Layout.fillWidth: true; model: [Theme.t("保留源类型", "Preserve source type", "保留來源類型"), "uint8", "uint16", "half", "float32"]; onActivated: root.edited(); Accessible.name: Theme.t("输出精度", "Output precision", "輸出精度") }
            Label { text: Theme.t("冲突", "Conflict", "衝突"); color: Theme.text }
            ComboBox { id: conflict; Layout.fillWidth: true; model: [Theme.t("跳过", "Skip", "略過"), Theme.t("重命名", "Rename", "重新命名"), Theme.t("覆盖", "Overwrite", "覆寫")]; onActivated: root.edited(); Accessible.name: Theme.t("冲突策略", "Conflict policy", "衝突策略") }
            Label { text: Theme.t("子资源索引", "Subresource index", "子資源索引"); color: Theme.text }
            SpinBox { id: resource; Layout.fillWidth: true; from: 0; to: 65535; editable: true; onValueModified: root.edited(); Accessible.name: Theme.t("显式子资源索引", "Explicit subresource index", "明確子資源索引") }
        }
        CheckBox { id: overwrite; text: Theme.t("允许在上述输出范围内覆盖已有文件", "Allow overwrite within the output scope above", "允許在上述輸出範圍內覆寫現有檔案"); onToggled: root.edited() }
        ConversionInputs { Layout.fillWidth: true; controller: root.controller; scope: ({input_roots: [input.text], output_roots: [output.text], allow_overwrite: overwrite.checked}); onAccepted: paths => { root.chooseInputs(paths); root.edited() } }
        ConversionOverrides { id: perItem; Layout.fillWidth: true; inputs: root.inputs; inputInfo: root.controller.imageJobs.inputInfo; onEdited: root.edited() }
        Tool { id: advanced; objectName: "conversionAdvanced"; checkable: true; text: (checked ? "▾ " : "▸ ") + Theme.t("Alpha、数值与编码规则", "Alpha, numeric and encoding rules", "Alpha、數值與編碼規則") }
        ColumnLayout {
            Layout.fillWidth: true; visible: advanced.checked
            Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("默认逐文件保留真实通道。缺少所需通道、未定义 Alpha 或不兼容类型将使预检失败；不会静默补通道或归一化。", "Preserves each file's real channels. Missing channels, undefined alpha or incompatible types fail preflight; channels and normalization are never silently invented.", "預設逐檔保留真實通道。缺少通道、未定義 Alpha 或不相容類型會使預檢失敗；不會自動補通道或正規化。") }
            ComboBox { id: alphaAction; objectName: "conversionAlphaAction"; Layout.fillWidth: true; model: [Theme.t("保留 Alpha", "Preserve alpha", "保留 Alpha"), Theme.t("删除 Alpha，保留 RGB", "Drop alpha, keep RGB", "移除 Alpha，保留 RGB"), Theme.t("在线性背景上合成", "Composite on linear background", "在線性背景上合成")]; onActivated: root.edited(); Accessible.name: Theme.t("Alpha 处理", "Alpha action", "Alpha 處理") }
            ComboBox { id: sourceAlpha; Layout.fillWidth: true; model: [Theme.t("使用源 Alpha 标记", "Use source alpha tag", "使用來源 Alpha 標記"), Theme.t("无 Alpha", "No alpha", "無 Alpha"), Theme.t("直通", "Straight", "直通"), Theme.t("预乘", "Premultiplied", "預乘")]; onActivated: root.edited(); Accessible.name: Theme.t("源 Alpha 解释", "Source alpha interpretation", "來源 Alpha 解釋") }
            CheckBox { id: colour; text: Theme.t("将输入明确解释为颜色", "Explicitly interpret input as colour", "將輸入明確解釋為色彩"); onToggled: root.edited() }
            ComboBox { id: transfer; Layout.fillWidth: true; model: [Theme.t("不变换", "No transfer", "不變換"), "Linear → sRGB", "sRGB → Linear"]; enabled: colour.checked; onActivated: root.edited(); Accessible.name: Theme.t("颜色变换", "Colour transfer", "色彩變換") }
            RowLayout { Label { text: "EV"; color: Theme.text } ExposureSpinBox { id: exposure; enabled: colour.checked; onValueModified: root.edited(); Accessible.name: "EV" } Label { text: Theme.t("线性背景 %", "Linear background %", "線性背景 %"); color: Theme.text } SpinBox { id: background; from: 0; to: 100; value: 18; editable: true; onValueModified: root.edited(); Accessible.name: Theme.t("线性背景百分比", "Linear background percent", "線性背景百分比") } }
            CheckBox { id: quantize; objectName: "conversionQuantize"; text: Theme.t("允许裁切至 [0,1] 并四舍五入量化（报告损失）", "Allow clamp to [0,1] and round-half-up quantization (report losses)", "允許裁切至 [0,1] 並四捨五入量化（報告損失）"); onToggled: root.edited() }
            CheckBox { id: replaceNonFinite; text: Theme.t("将 NaN / Inf 明确替换为 0（报告数量）", "Explicitly replace NaN / Inf with 0 (report counts)", "將 NaN / Inf 明確替換為 0（報告數量）"); onToggled: root.edited() }
            CheckBox { id: flatten; text: Theme.t("将选定 dataWindow 展平到原点（报告窗口损失）", "Flatten selected dataWindow to origin (report window loss)", "將選定 dataWindow 平面化至原點（報告視窗損失）"); onToggled: root.edited() }
            CheckBox { id: dropMetadata; text: Theme.t("明确丢弃元数据（报告损失）", "Explicitly drop metadata (report losses)", "明確捨棄中繼資料（報告損失）"); onToggled: root.edited() }
            CheckBox { id: allowLossy; objectName: "conversionAllowLossy"; text: Theme.t("允许有损编码；仍须确认实际损失", "Allow lossy encoding; actual losses still require approval", "允許有損編碼；仍須確認實際損失"); onToggled: root.edited() }
            RowLayout { visible: ["jpeg", "webp"].includes(root.targets[target.currentIndex]); Label { text: Theme.t("质量", "Quality", "品質"); color: Theme.text } SpinBox { id: quality; from: 1; to: 100; value: 90; onValueModified: root.edited(); Accessible.name: Theme.t("编码质量", "Encoding quality", "編碼品質") } }
            CheckBox { id: webpLossless; visible: root.targets[target.currentIndex] === "webp"; checked: true; text: Theme.t("WebP 无损", "WebP lossless", "WebP 無損"); onToggled: root.edited() }
            RowLayout { visible: root.targets[target.currentIndex] === "png"; Label { text: Theme.t("PNG 压缩", "PNG compression", "PNG 壓縮"); color: Theme.text } SpinBox { id: pngCompression; from: 0; to: 9; value: 6; onValueModified: root.edited(); Accessible.name: Theme.t("PNG 压缩级别", "PNG compression level", "PNG 壓縮等級") } }
            ComboBox { id: tiffCompression; visible: root.targets[target.currentIndex] === "tiff"; model: ["None", "LZW", "Deflate", "Packbits"]; currentIndex: 2; onActivated: root.edited(); Accessible.name: Theme.t("TIFF 压缩", "TIFF compression", "TIFF 壓縮") }
            CheckBox { id: exrTiled; visible: root.targets[target.currentIndex] === "exr"; text: "EXR · 64×64 tiled / ZIP"; onToggled: root.edited() }
            ComboBox { id: ddsFormat; Layout.fillWidth: true; visible: root.targets[target.currentIndex] === "dds"; textRole: "name"; valueRole: "code"; model: [{name: "Native", code: 0}, {name: "RGBA8 UNorm", code: 28}, {name: "RGBA32 Float", code: 2}, {name: "BC1 UNorm", code: 71}, {name: "BC3 UNorm", code: 77}, {name: "BC5 UNorm", code: 83}, {name: "BC5 SNorm", code: 84}, {name: "BC6H UF16", code: 95}, {name: "BC7 UNorm", code: 98}]; onActivated: root.edited(); Accessible.name: Theme.t("DDS 格式", "DDS format", "DDS 格式") }
        }
    }
    }
    Tool { objectName: "createImagePlan"; text: root.controller.imageJobs.busy ? Theme.t("正在准备…", "Preparing…", "正在準備…") : Theme.t("预检，不写出文件", "Preflight without writing files", "預檢，不寫出檔案"); enabled: !root.controller.imageJobs.busy && root.inputs.length > 0 && !!input.text && !!output.text && (conflict.currentIndex !== 2 || overwrite.checked); onClicked: root.preflight({input_roots: [input.text], output_roots: [output.text], allow_overwrite: overwrite.checked}, root.options(), naming.text, ["skip", "rename", "overwrite"][conflict.currentIndex]) }
    FolderDialog { id: inputFolder; onAccepted: { input.text = root.controller.localFile(selectedFolder); root.edited() } }
    FolderDialog { id: outputFolder; onAccepted: { output.text = root.controller.localFile(selectedFolder); root.edited() } }
}
