pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
ColumnLayout {
    id: root
    objectName: "conversionSettings"
    required property var controller
    required property var inputs
    property bool modelMode: false
    property string exportPreset: ""
    property int abcSample: -1
    property real abcSeconds: 0
    property string abcSourceSha256: ""
    property bool quickMode: false
    property bool moreOptions: false
    readonly property bool detailed: !quickMode || moreOptions
    property alias outputDirectory: output.text
    readonly property var inputRoots: controller.imageJobs.localInputRoots
    readonly property bool inputScopeMissing: inputRoots.length === 0 || inputRoots.some(p => typeof p !== "string" || !p.trim())
    readonly property bool outputScopeMissing: !output.text.trim()
    readonly property var scope: ({input_roots: inputRoots, output_roots: [output.text || inputRoots[0] || ""], allow_overwrite: overwrite.checked})
    function itemOverrides(): var { return modelMode || quickMode ? [] : perItem.options() }
    signal back()
    signal edited()
    signal chooseInputs(var paths)
    signal preflight(var scope, var options, string nameTemplate, string conflict, var metadata)
    readonly property var targets: controller.imageJobs.capabilities.write_targets
    readonly property var namingPreview: controller.imageJobs.nameExamples(root.inputs, root.modelMode && modelFields.contentValue === "model" ? naming.nameTemplate.replace(/\{clip\}/g, root.abcSample >= 0 ? "frame" + root.abcSample : "model") : naming.nameTemplate, root.modelMode ? "fbx" : root.targets[target.currentIndex] || "", root.itemOverrides())
    function options(): var {
        if (modelMode) return modelFields.options()
        const o = {target: targets[target.currentIndex], subresource: resource.value,
            alpha_action: ["preserve", "drop", "composite"][alphaAction.currentIndex],
            meaning: colour.checked ? "colour" : "data", transfer: colour.checked ? ["preserve", "linear_to_srgb", "srgb_to_linear"][transfer.currentIndex] : "preserve",
            exposure: colour.checked ? exposure.realValue : 0, background_linear: [background.value / 100, background.value / 100, background.value / 100],
            quantization: quantize.checked ? "clamp_round" : "reject_loss", non_finite: replaceNonFinite.checked ? "replace_zero" : ([1, 2].includes(depth.currentIndex) ? "reject" : "preserve"),
            window: flatten.checked ? "selected_data" : "preserve", metadata: dropMetadata.checked ? "drop" : "preserve_supported",
            allow_lossy: allowLossy.checked, quality: quality.value, png_compression: pngCompression.value,
            webp_lossless: webpLossless.checked, exr_tiled: exrTiled.checked,
            tiff_compression: ["none", "lzw", "deflate", "packbits"][tiffCompression.currentIndex], dds_format: ddsFormat.currentValue || 0}
        if (depth.currentIndex > 0) o.output_scalar = ["", "uint8", "uint16", "half", "float32"][depth.currentIndex]
        if (sourceAlpha.currentIndex > 0) o.source_alpha = ["", "absent", "straight", "premultiplied"][sourceAlpha.currentIndex]
        return o
    }
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() { if (root.visible) Qt.callLater(() => { const window = root.Window.window; if (window) { Theme.revealIn(settingsScroll, window.activeFocusItem); Theme.revealIn(summaryScroll, window.activeFocusItem) } }) }
    }
    RowLayout {
        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 24
        ScrollView {
            id: settingsScroll; objectName: "conversionSettingsScroll"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 65; clip: true; contentWidth: availableWidth
            ColumnLayout { width: settingsScroll.availableWidth; spacing: 20
FormGroup { objectName: "conversionSources"; title: Theme.t("来源与输出", "Source and output", "來源與輸出"); Layout.fillWidth: true
 ColumnLayout { anchors.fill: parent; spacing: 12
 ConversionSources { visible: !root.modelMode && !root.quickMode; controller: root.controller }
 RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; spacing: 12
     visible: root.modelMode && !root.quickMode
     Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("输入文件", "Input files", "輸入檔案") }
     Label { font.pixelSize: Theme.formText; objectName: "conversionInputCount"; Layout.fillWidth: true; color: Theme.text; text: Theme.t("当前选择 · ", "Selected · ", "目前選取 · ") + root.inputs.length + (root.modelMode ? Theme.t(" 个输入", " inputs", " 個輸入") : Theme.t(" 个图片", " images", " 個影像")) }
     Tool { form: true; visible: root.modelMode; text: Theme.t("选择文件", "Choose files", "選擇檔案"); onClicked: modelFiles.open() }
     Tool { form: true; id: inputList; objectName: "conversionInputList"; quiet: true; checkable: true; iconName: "expand"; iconRotation: checked ? 180 : 0; text: Theme.t("查看列表", "View list", "檢視清單") }
 }
 Readout { form: true; visible: root.modelMode && inputList.checked && !root.quickMode; Layout.fillWidth: true; label: Theme.t("完整路径 · 可选择复制", "Full paths · Select to copy", "完整路徑 · 可選取複製"); value: root.inputs.join("\n") || "—" }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("输出目录", "Output folder", "輸出資料夾") } Field { form: true; id: output; objectName: "conversionOutputRoot"; Layout.fillWidth: true; onTextEdited: root.edited(); Accessible.name: Theme.t("输出目录", "Output directory", "輸出目錄") } Tool { objectName: "chooseConversionOutput"; form: true; iconName: "folder"; Accessible.name: Theme.t("选择输出目录", "Choose output folder", "選擇輸出資料夾"); onClicked: { outputFolder.currentFolder = root.controller.existingFolderUrl(output.text); outputFolder.open() } } }
Label { objectName: "conversionOutputScopeHint"; visible: root.outputScopeMissing; Layout.fillWidth: true; font.pixelSize: Theme.noteText; color: Theme.warning; wrapMode: Text.WordWrap; text: Theme.t("请选择输出目录后再开始预检。", "Choose an output folder before preflighting.", "請選擇輸出資料夾後再開始預檢。") }
RowLayout { visible: root.detailed; Layout.fillWidth: true
    Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("任务名称", "Task name", "工作名稱") }
    Field { id: taskName; objectName: "conversionTaskName"; form: true; Layout.fillWidth: true; maximumLength: 200; placeholderText: Theme.t("留空自动使用文件名和提交时间", "Automatic: filename and submission time", "留空自動使用檔名和提交時間"); onTextEdited: root.edited() }
}
RowLayout { visible: root.detailed; Layout.fillWidth: true
    Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("任务分类", "Category", "工作分類") }
    Field { id: taskCategory; objectName: "conversionTaskCategory"; form: true; Layout.fillWidth: true; maximumLength: 80; placeholderText: root.modelMode ? Theme.t("模型与动画（可自定义）", "Models and animation (customizable)", "模型與動畫（可自訂）") : Theme.t("图片（可自定义）", "Images (customizable)", "影像（可自訂）"); onTextEdited: root.edited() }
}
ConversionNaming { id: naming; visible: root.detailed; controller: root.controller; Layout.fillWidth: true; modelMode: root.modelMode; multipleStacks: modelFields.multipleStacks; onEdited: root.edited() }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("冲突策略", "Conflict policy", "衝突策略") } Choice { form: true; id: conflict; notes: [Theme.t("已有目标保持不变，此项跳过。", "Keep the existing destination and skip this item.", "現有目標保持不變，此項略過。"), Theme.t("保留已有目标，为新输出分配不同名称。", "Keep existing files and resolve a new output name.", "保留現有目標，為新輸出分配不同名稱。"), Theme.t("需勾选允许覆盖；源文件仍受保护。", "Requires overwrite permission; source files remain protected.", "需勾選允許覆寫；來源檔案仍受保護。")]; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; labels: [Theme.t("跳过", "Skip", "略過"), Theme.t("重命名", "Rename", "重新命名"), Theme.t("覆盖", "Overwrite", "覆寫")]; onActivated: root.edited(); Accessible.name: Theme.t("冲突策略", "Conflict policy", "衝突策略") } }
OptionCheck { Layout.fillWidth: true; id: overwrite; visible: conflict.currentIndex === 2; text: Theme.t("允许在上述输出范围内覆盖已有文件", "Allow overwrite within the output scope above", "允許在上述輸出範圍內覆寫現有檔案"); onToggled: root.edited() }
 }
}
ModelExportSettings { id: modelFields; visible: root.modelMode; Layout.fillWidth: true; controller: root.controller; inputs: root.inputs; scope: root.scope; animationOnlyPreset: root.exportPreset === "animation"; abcSample: root.abcSample; abcSeconds: root.abcSeconds; abcSourceSha256: root.abcSourceSha256; compact: root.quickMode && !root.moreOptions; onEdited: root.edited() }
FormGroup { visible: !root.modelMode; objectName: "conversionEncoding"; title: Theme.t("编码设置", "Encoding settings", "編碼設定"); Layout.fillWidth: true
 ColumnLayout { anchors.fill: parent; spacing: 12
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("目标格式", "Target format", "目標格式") } Choice { form: true; id: target; notes: root.targets.map(t => ({png: Theme.t("无损整数图像，支持 Alpha。", "Lossless integer image with alpha.", "無損整數影像，支援 Alpha。"), tga: Theme.t("常用 8 位通道，支持 Alpha。", "Commonly 8-bit channels with alpha.", "常用 8 位通道，支援 Alpha。"), tiff: Theme.t("支持多种数值类型；按实际编码能力预检。", "Multiple numeric types; preflight checks encoder support.", "支援多種數值類型；按實際編碼能力預檢。"), exr: Theme.t("高动态范围，支持半精度与单精度浮点。", "HDR with half and single-precision floats.", "高動態範圍，支援半精度與單精度浮點。"), hdr: Theme.t("Radiance RGBE；不支持 Alpha、负值，编码会损失精度。", "Radiance RGBE: no alpha or negative values; encoding loses precision.", "Radiance RGBE；不支援 Alpha、負值，編碼會損失精度。")}[t] || Theme.t("按源通道、数值范围与编码能力预检。", "Preflight checks source channels, range and encoder support.", "按來源通道、數值範圍與編碼能力預檢。"))); objectName: "conversionTarget"; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; model: root.targets.map(t => t.toUpperCase()); onActivated: root.edited(); Accessible.name: Theme.t("目标格式", "Target format", "目標格式") } }
RowLayout { visible: root.detailed; Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("位深", "Precision", "位深") } Choice { form: true; id: depth; notes: [Theme.t("尽可能保留源数值类型；不兼容时预检阻止。", "Keep source numeric type where supported; preflight blocks incompatible types.", "盡可能保留來源數值類型；不相容時預檢阻止。"), Theme.t("8 位无符号整数；浮点转整数可能裁切与量化。", "8-bit unsigned integer; float conversion can clamp and quantize.", "8 位無符號整數；浮點轉整數可能裁切與量化。"), Theme.t("16 位无符号整数。", "16-bit unsigned integer.", "16 位無符號整數。"), Theme.t("16 位半精度浮点；范围与精度有限。", "16-bit half float with limited range and precision.", "16 位半精度浮點；範圍與精度有限。"), Theme.t("32 位单精度浮点。", "32-bit single-precision float.", "32 位單精度浮點。")]; objectName: "conversionDepth"; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; labels: [Theme.t("保留源类型", "Preserve source type", "保留來源類型"), "uint8", "uint16", "half", "float32"]; onActivated: root.edited(); Accessible.name: Theme.t("输出精度", "Output precision", "輸出精度") } }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 40; visible: root.detailed && root.targets[target.currentIndex] === "png"; Label { font.pixelSize: Theme.formText; Layout.preferredWidth: 148; wrapMode: Text.WordWrap; text: Theme.t("PNG 无损压缩级别", "PNG lossless compression", "PNG 無損壓縮等級"); color: Theme.text } NumberField { form: true; id: pngCompression; objectName: "conversionPngCompression"; Layout.preferredWidth: 160; Layout.maximumWidth: 180; Layout.fillWidth: true; from: 0; to: 9; value: 0; onValueModified: root.edited(); Accessible.name: Theme.t("PNG 无损压缩级别", "PNG lossless compression level", "PNG 無損壓縮等級") } }
Label { objectName: "pngCompressionExplanation"; visible: root.detailed && root.targets[target.currentIndex] === "png"; Layout.fillWidth: true; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("默认 0：不压缩，保留像素值、位深和 Alpha。1–9 为可选无损压缩，只改变编码速度和文件大小，不改变画质。", "Default 0: no compression; pixel values, bit depth and alpha are preserved. Optional levels 1–9 use lossless compression, changing encoding speed and file size, not image quality.", "預設 0：不壓縮，保留像素值、位元深度與 Alpha。1–9 為可選無損壓縮，只改變編碼速度與檔案大小，不改變畫質。") }
RowLayout { visible: ["jpeg", "webp"].includes(root.targets[target.currentIndex]); Label { font.pixelSize: Theme.formText; text: Theme.t("质量", "Quality", "品質"); color: Theme.text } NumberField { form: true; id: quality; from: 1; to: 100; value: 90; onValueModified: root.edited(); Accessible.name: Theme.t("编码质量", "Encoding quality", "編碼品質") } }
OptionCheck { Layout.fillWidth: true; id: webpLossless; visible: root.targets[target.currentIndex] === "webp"; checked: true; text: Theme.t("WebP 无损", "WebP lossless", "WebP 無損"); onToggled: root.edited() }
Choice { form: true; id: tiffCompression; notes: [Theme.t("不压缩。", "Uncompressed.", "不壓縮。"), Theme.t("LZW 无损压缩。", "Lossless LZW compression.", "LZW 無損壓縮。"), Theme.t("Deflate 无损压缩。", "Lossless Deflate compression.", "Deflate 無損壓縮。"), Theme.t("PackBits 无损游程压缩。", "Lossless PackBits run-length compression.", "PackBits 無損遊程壓縮。")]; visible: root.targets[target.currentIndex] === "tiff"; model: ["None", "LZW", "Deflate", "Packbits"]; currentIndex: 2; onActivated: root.edited(); Accessible.name: Theme.t("TIFF 压缩", "TIFF compression", "TIFF 壓縮") }
OptionCheck { Layout.fillWidth: true; id: exrTiled; visible: root.targets[target.currentIndex] === "exr"; text: "EXR · 64×64 tiled / ZIP"; onToggled: root.edited() }
Choice { form: true; id: ddsFormat; notes: ["Source DXGI format", "8-bit unsigned normalized RGBA", "32-bit float RGBA", "BC1 · lossy RGB / 1-bit alpha", "BC3 · lossy RGBA", "BC5 · lossy two-channel unsigned", "BC5 · lossy two-channel signed", "BC6H · lossy unsigned HDR RGB", "BC7 · lossy RGBA"]; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; visible: root.targets[target.currentIndex] === "dds"; textRole: "name"; valueRole: "code"; model: [{name: "Native", code: 0}, {name: "RGBA8 UNorm", code: 28}, {name: "RGBA32 Float", code: 2}, {name: "BC1 UNorm", code: 71}, {name: "BC3 UNorm", code: 77}, {name: "BC5 UNorm", code: 83}, {name: "BC5 SNorm", code: 84}, {name: "BC6H UF16", code: 95}, {name: "BC7 UNorm", code: 98}]; onActivated: root.edited(); Accessible.name: Theme.t("DDS 格式", "DDS format", "DDS 格式") }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: "Alpha" } Choice { form: true; id: alphaAction; notes: [Theme.t("保留源 Alpha；目标不支持时阻止输出。", "Keep source alpha; unsupported targets block output.", "保留來源 Alpha；目標不支援時阻止輸出。"), Theme.t("删除透明度，保留 RGB；预检报告损失。", "Drop transparency and retain RGB; preflight reports loss.", "移除透明度，保留 RGB；預檢報告損失。"), Theme.t("按线性背景色合成后移除 Alpha。", "Composite on the linear background, then remove alpha.", "按線性背景色合成後移除 Alpha。")]; objectName: "conversionAlphaAction"; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; labels: [Theme.t("保留 Alpha", "Preserve alpha", "保留 Alpha"), Theme.t("删除 Alpha，保留 RGB", "Drop alpha, keep RGB", "移除 Alpha，保留 RGB"), Theme.t("在线性背景上合成", "Composite on linear background", "在線性背景上合成")]; onActivated: root.edited(); Accessible.name: Theme.t("Alpha 处理", "Alpha action", "Alpha 處理") } }
}
}
SectionHeader { form: true; background: Rectangle { color: Theme.raised; radius: Theme.groupRadius; border.color: Theme.line } id: advancedSettings; visible: !root.modelMode && !root.quickMode; objectName: "conversionAdvancedSettings"; Layout.fillWidth: true; text: Theme.t("高级设置", "Advanced settings", "進階設定") }
ColumnLayout { Layout.fillWidth: true; visible: !root.modelMode && (root.quickMode ? root.moreOptions : advancedSettings.checked); spacing: 20
FormGroup { objectName: "conversionContent"; title: Theme.t("内容选择", "Content selection", "內容選擇"); Layout.fillWidth: true
 ColumnLayout { anchors.fill: parent; spacing: 12
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("子资源索引", "Subresource index", "子資源索引") } NumberField { form: true; id: resource; Layout.preferredWidth: 160; Layout.maximumWidth: 180; Layout.fillWidth: true; from: 0; to: 65535; editable: true; onValueModified: root.edited(); Accessible.name: Theme.t("显式子资源索引", "Explicit subresource index", "明確子資源索引") } }
ConversionOverrides { visible: !root.quickMode; id: perItem; Layout.fillWidth: true; inputs: root.inputs; inputInfo: root.controller.imageJobs.inputInfo; onEdited: root.edited() }
 Label { Layout.fillWidth: true; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("默认逐文件保留真实通道；不存在的通道或不兼容类型会在预检中阻止输出。", "Preserves each file's source channels. Absent channels and incompatible types block output in preflight.", "預設逐檔保留實際通道；不存在的通道或不相容類型會在預檢中阻止輸出。") }
 }
}
FormGroup { title: Theme.t("数值与元数据", "Numeric rules and metadata", "數值與中繼資料"); Layout.fillWidth: true
ColumnLayout { anchors.fill: parent; spacing: 12
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("源 Alpha 类型", "Source alpha type", "來源 Alpha 類型") } Choice { form: true; id: sourceAlpha; notes: [Theme.t("保留源文件的类型标记，未标记则保留未知。仅在运算或目标格式需要时要求明确类型。", "Keep the source type tag, including unknown. An explicit type is required only by relevant operations or the target format.", "保留來源檔案的類型標記，未標記則保留未知。僅在運算或目標格式需要時要求明確類型。"), Theme.t("声明源数据没有 Alpha，不会删除已有 A 通道；移除透明度请使用上方 Alpha 处理。", "Declare that the source has no alpha; this does not remove an A channel. To remove transparency, use Alpha action above.", "宣告來源資料沒有 Alpha，不會移除現有 A 通道；移除透明度請使用上方 Alpha 處理。"), Theme.t("RGB 未乘 Alpha。", "RGB is not multiplied by alpha.", "RGB 未乘 Alpha。"), Theme.t("RGB 已乘 Alpha。", "RGB is multiplied by alpha.", "RGB 已乘 Alpha。")]; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; labels: [Theme.t("保留源类型（可未知）", "Keep source type (may be unknown)", "保留來源類型（可未知）"), Theme.t("无 Alpha", "No alpha", "無 Alpha"), Theme.t("直通", "Straight", "直通"), Theme.t("预乘", "Premultiplied", "預乘")]; onActivated: root.edited(); Accessible.name: Theme.t("源 Alpha 解释", "Source alpha interpretation", "來源 Alpha 解釋") } }
OptionCheck { Layout.fillWidth: true; id: colour; objectName: "conversionColour"; text: Theme.t("将输入明确解释为颜色", "Explicitly interpret input as colour", "將輸入明確解釋為色彩"); onToggled: root.edited() }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("颜色变换", "Colour transfer", "色彩變換") } Choice { form: true; id: transfer; objectName: "conversionTransfer"; notes: [Theme.t("不改变传递函数。", "Do not change the transfer function.", "不改變傳遞函數。"), Theme.t("线性颜色编码为 sRGB；不适合数据贴图。", "Encode linear colour as sRGB; not for data textures.", "線性色彩編碼為 sRGB；不適合資料貼圖。"), Theme.t("sRGB 解码为线性颜色；不适合数据贴图。", "Decode sRGB to linear colour; not for data textures.", "sRGB 解碼為線性色彩；不適合資料貼圖。")]; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; labels: [Theme.t("不变换", "No transfer", "不變換"), "Linear → sRGB", "sRGB → Linear"]; enabled: colour.checked; onActivated: root.edited(); Accessible.name: Theme.t("颜色变换", "Colour transfer", "色彩變換") } }
OptionCheck { Layout.fillWidth: true; id: quantize; objectName: "conversionQuantize"; text: Theme.t("允许裁切至 [0,1] 并四舍五入量化（报告损失）", "Allow clamp to [0,1] and round-half-up quantization (report losses)", "允許裁切至 [0,1] 並四捨五入量化（報告損失）"); onToggled: root.edited() }
OptionCheck { Layout.fillWidth: true; id: replaceNonFinite; text: Theme.t("将 NaN / Inf 明确替换为 0（报告数量）", "Explicitly replace NaN / Inf with 0 (report counts)", "將 NaN / Inf 明確替換為 0（報告數量）"); onToggled: root.edited() }
OptionCheck { Layout.fillWidth: true; id: allowLossy; objectName: "conversionAllowLossy"; text: Theme.t("允许有损编码；仍须确认实际损失", "Allow lossy encoding; actual losses still require approval", "允許有損編碼；仍須確認實際損失"); onToggled: root.edited() }
SectionHeader { form: true; id: advanced; objectName: "conversionAdvanced"; Layout.fillWidth: true; text: Theme.t("曝光、窗口与元数据", "Exposure, window and metadata", "曝光、視窗與中繼資料") }
 ColumnLayout { Layout.fillWidth: true; visible: advanced.checked
RowLayout { Label { font.pixelSize: Theme.formText; text: "EV"; color: Theme.text } ExposureSpinBox { id: exposure; objectName: "conversionExposure"; enabled: colour.checked; onValueModified: root.edited(); Accessible.name: "EV" } Label { font.pixelSize: Theme.formText; text: Theme.t("线性背景 %", "Linear background %", "線性背景 %"); color: Theme.text } NumberField { form: true; id: background; from: 0; to: 100; value: 18; editable: true; onValueModified: root.edited(); Accessible.name: Theme.t("线性背景百分比", "Linear background percent", "線性背景百分比") } }
OptionCheck { Layout.fillWidth: true; id: flatten; text: Theme.t("将选定 dataWindow 展平到原点（报告窗口损失）", "Flatten selected dataWindow to origin (report window loss)", "將選定 dataWindow 平面化至原點（報告視窗損失）"); onToggled: root.edited() }
OptionCheck { Layout.fillWidth: true; id: dropMetadata; objectName: "conversionMetadata"; text: Theme.t("明确丢弃元数据（报告损失）", "Explicitly drop metadata (report losses)", "明確捨棄中繼資料（報告損失）"); onToggled: root.edited() }
}
}
}
}
}
}

ScrollView {
    id: summaryScroll; visible: !root.quickMode; Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 35; clip: true; contentWidth: availableWidth
FormGroup {
    objectName: "conversionSummary"; width: summaryScroll.availableWidth
    title: Theme.t("转换摘要", "Conversion summary", "轉換摘要")
    ColumnLayout { anchors.fill: parent; spacing: 12
        Readout { form: true; inlineValue: true; Layout.fillWidth: true; Layout.minimumHeight: 24; label: Theme.t("输入数量", "Input count", "輸入數量"); value: root.inputs.length + Theme.t(" 个输入", " inputs", " 個輸入") }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        Readout { form: true; inlineValue: true; Layout.fillWidth: true; label: Theme.t("输出格式", "Output format", "輸出格式"); value: root.modelMode ? "FBX · " + modelFields.contentLabel : target.displayText + " · " + depth.displayText }
        Readout { form: true; objectName: "conversionContentSource"; inlineValue: true; Layout.fillWidth: true; label: Theme.t("内容来源", "Content source", "內容來源"); value: Theme.t("源内容 · 不套用预览效果", "Source content · No preview effects", "來源內容 · 不套用預覽效果") }
        Readout { form: true; objectName: "conversionPreflightStatus"; inlineValue: true; Layout.fillWidth: true; label: Theme.t("预检状态", "Preflight status", "預檢狀態"); value: root.controller.imageJobs.busy ? Theme.t("正在准备…", "Preparing…", "正在準備…") : root.controller.imageJobs.error ? Theme.t("预检失败", "Preflight failed", "預檢失敗") : root.controller.imageJobs.plan.state ? Theme.state(root.controller.imageJobs.plan.state) : Theme.t("待预检", "Not preflighted", "待預檢") }
        Readout { form: true; inlineValue: true; Layout.fillWidth: true; visible: !root.modelMode; label: Theme.t("Alpha 处理", "Alpha action", "Alpha 處理"); value: alphaAction.displayText }
        Readout { form: true; visible: root.modelMode; inlineValue: true; Layout.fillWidth: true; label: Theme.t("坐标与单位", "Axes and units", "座標與單位"); value: modelFields.coordinateLabel }
        Readout { form: true; inlineValue: true; Layout.fillWidth: true; label: Theme.t("冲突策略", "Conflict policy", "衝突策略"); value: conflict.displayText }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        Readout { form: true; objectName: "conversionNamingExamples"; Layout.fillWidth: true; label: Theme.t("命名示例 · 预检前", "Name examples · Before preflight", "命名範例 · 預檢前"); value: Explain.errorText(root.namingPreview.error) || (root.namingPreview.names || []).join("\n") || "—" }
        RowLayout { Layout.fillWidth: true; spacing: 10
            AppIcon { name: "preflight"; variant: "muted"; Layout.alignment: Qt.AlignTop }
            Label { font.pixelSize: Theme.formText; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("预检会检查输入、输出范围与格式兼容性，再确认转换。", "Preflight checks inputs, output scope and format compatibility before conversion.", "預檢會檢查輸入、輸出範圍與格式相容性，再確認轉換。") }
        }
    }
}
}
}
    Readout { visible: root.quickMode; Layout.fillWidth: true; label: Theme.t("命名预览", "Naming preview", "命名預覽"); value: Explain.errorText(root.namingPreview.error) || (root.namingPreview.names || []).join("\n") || "—" }
    Tool { visible: root.quickMode; checkable: true; checked: root.moreOptions; text: Theme.t("更多选项", "More options", "更多選項"); onClicked: root.moreOptions = checked }
    RowLayout {
        objectName: "conversionFooter"; Layout.fillWidth: true; Layout.minimumHeight: 40
        Label { Layout.fillWidth: true; font.pixelSize: Theme.formText; color: Theme.muted; wrapMode: Text.WordWrap; text: root.modelMode ? Theme.t("逐项检查并导出，失败会列出原因；不套用预览效果。", "Check and export each item; failures include reasons. Preview effects are excluded.", "逐項檢查並匯出，失敗會列出原因；不套用預覽效果。") : Theme.t("预检不写出文件；不套用视口曝光、背景或平铺。", "Preflight writes no files. Viewport exposure, background and tiling are excluded.", "預檢不寫出檔案；不套用視口曝光、背景或平鋪。") }
        Tool { form: true; text: Theme.t("取消", "Cancel", "取消"); onClicked: root.back() }
Tool { form: true; iconName: "preflight"; primary: true; objectName: "createImagePlan"; text: root.controller.imageJobs.busy ? Theme.t("正在准备…", "Preparing…", "正在準備…") : (root.quickMode || root.modelMode) ? Theme.t("导出", "Export", "匯出") : Theme.t("开始预检", "Start preflight", "開始預檢"); enabled: !root.controller.imageJobs.busy && root.inputs.length > 0 && !root.inputScopeMissing && !root.outputScopeMissing && !root.controller.imageJobs.importing && !root.namingPreview.error && (conflict.currentIndex !== 2 || overwrite.checked); onClicked: root.preflight({input_roots: root.inputRoots, output_roots: [output.text], allow_overwrite: overwrite.checked}, root.options(), naming.nameTemplate, ["skip", "rename", "overwrite"][conflict.currentIndex], {name: taskName.text, category: taskCategory.text}) }
    }
FileDialog { id: modelFiles; fileMode: FileDialog.OpenFiles; nameFilters: ["Models (" + (root.controller.imageJobs.capabilities.model_export?.input_formats || []).map(x => "*." + x).join(" ") + ")"]; onAccepted: { root.chooseInputs(selectedFiles.map(f => root.controller.localFile(f))); root.edited() } }
FolderDialog { id: outputFolder; objectName: "conversionOutputDialog"; onAccepted: { output.text = root.controller.localFile(selectedFolder); root.edited() } }
}
