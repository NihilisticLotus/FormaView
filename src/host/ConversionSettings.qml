pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
ColumnLayout {
    id: root
    required property var controller
    required property var inputs
    property alias inputRoot: input.text
    function itemOverrides(): var { return perItem.options() }
    signal back()
    signal edited()
    signal chooseInputs(var paths)
    signal preflight(var scope, var options, string nameTemplate, string conflict)
    readonly property var targets: controller.imageJobs.capabilities.write_targets
    readonly property var namingPreview: controller.imageJobs.nameExamples(root.inputs, naming.text, root.targets[target.currentIndex] || "", perItem.options())
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
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() { if (root.visible) Qt.callLater(() => { const window = root.Window.window; if (window) { Theme.revealIn(settingsScroll, window.activeFocusItem); Theme.revealIn(summaryScroll, window.activeFocusItem) } }) }
    }
    RowLayout {
        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 24
        ScrollView {
            id: settingsScroll; objectName: "conversionSettingsScroll"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 65; clip: true; contentWidth: availableWidth
            ColumnLayout { width: settingsScroll.availableWidth; spacing: 24
FormGroup { objectName: "conversionSources"; title: Theme.t("来源与输出", "Source and output", "來源與輸出"); Layout.fillWidth: true
 ColumnLayout { anchors.fill: parent; spacing: 16
 RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; spacing: 12
     Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("输入文件", "Input files", "輸入檔案") }
     Label { objectName: "conversionInputCount"; Layout.fillWidth: true; color: Theme.text; text: Theme.t("当前选择 · ", "Selected · ", "目前選取 · ") + root.inputs.length + Theme.t(" 个图片", " images", " 個影像") }
     Tool { id: inputList; objectName: "conversionInputList"; quiet: true; checkable: true; iconName: "expand"; iconRotation: checked ? 180 : 0; text: Theme.t("查看列表", "View list", "檢視清單") }
 }
 Readout { visible: inputList.checked; Layout.fillWidth: true; label: Theme.t("完整路径 · 可选择复制", "Full paths · Select to copy", "完整路徑 · 可選取複製"); value: root.inputs.join("\n") || "—" }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("输出目录", "Output folder", "輸出資料夾") } Field { id: output; objectName: "conversionOutputRoot"; Layout.fillWidth: true; onTextEdited: root.edited(); Accessible.name: Theme.t("输出目录", "Output directory", "輸出目錄") } Tool { iconName: "folder"; Accessible.name: Theme.t("选择输出目录", "Choose output folder", "選擇輸出資料夾"); onClicked: outputFolder.open() } }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("命名模板", "Filename template", "命名範本") } Field { id: naming; Accessible.description: "{name} / {index} / {ext}"; Layout.fillWidth: true; text: "{name}-converted.{ext}"; onTextEdited: root.edited(); Accessible.name: Theme.t("名称模板", "Filename template", "名稱範本") } }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("冲突策略", "Conflict policy", "衝突策略") } Choice { id: conflict; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; model: [Theme.t("跳过", "Skip", "略過"), Theme.t("重命名", "Rename", "重新命名"), Theme.t("覆盖", "Overwrite", "覆寫")]; onActivated: root.edited(); Accessible.name: Theme.t("冲突策略", "Conflict policy", "衝突策略") } }
OptionCheck { Layout.fillWidth: true; id: overwrite; visible: conflict.currentIndex === 2; text: Theme.t("允许在上述输出范围内覆盖已有文件", "Allow overwrite within the output scope above", "允許在上述輸出範圍內覆寫現有檔案"); onToggled: root.edited() }
 }
}
FormGroup { objectName: "conversionEncoding"; title: Theme.t("编码设置", "Encoding settings", "編碼設定"); Layout.fillWidth: true
 ColumnLayout { anchors.fill: parent; spacing: 16
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("目标格式", "Target format", "目標格式") } Choice { id: target; objectName: "conversionTarget"; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; model: root.targets.map(t => t.toUpperCase()); onActivated: root.edited(); Accessible.name: Theme.t("目标格式", "Target format", "目標格式") } }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("位深", "Precision", "位深") } Choice { id: depth; objectName: "conversionDepth"; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; model: [Theme.t("保留源类型", "Preserve source type", "保留來源類型"), "uint8", "uint16", "half", "float32"]; onActivated: root.edited(); Accessible.name: Theme.t("输出精度", "Output precision", "輸出精度") } }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 40; visible: root.targets[target.currentIndex] === "png"; Label { Layout.preferredWidth: 118; text: Theme.t("PNG 压缩", "PNG compression", "PNG 壓縮"); color: Theme.text } NumberField { id: pngCompression; Layout.preferredWidth: 160; Layout.maximumWidth: 180; Layout.fillWidth: true; from: 0; to: 9; value: 6; onValueModified: root.edited(); Accessible.name: Theme.t("PNG 压缩级别", "PNG compression level", "PNG 壓縮等級") } }
RowLayout { visible: ["jpeg", "webp"].includes(root.targets[target.currentIndex]); Label { text: Theme.t("质量", "Quality", "品質"); color: Theme.text } NumberField { id: quality; from: 1; to: 100; value: 90; onValueModified: root.edited(); Accessible.name: Theme.t("编码质量", "Encoding quality", "編碼品質") } }
OptionCheck { Layout.fillWidth: true; id: webpLossless; visible: root.targets[target.currentIndex] === "webp"; checked: true; text: Theme.t("WebP 无损", "WebP lossless", "WebP 無損"); onToggled: root.edited() }
Choice { id: tiffCompression; visible: root.targets[target.currentIndex] === "tiff"; model: ["None", "LZW", "Deflate", "Packbits"]; currentIndex: 2; onActivated: root.edited(); Accessible.name: Theme.t("TIFF 压缩", "TIFF compression", "TIFF 壓縮") }
OptionCheck { Layout.fillWidth: true; id: exrTiled; visible: root.targets[target.currentIndex] === "exr"; text: "EXR · 64×64 tiled / ZIP"; onToggled: root.edited() }
Choice { id: ddsFormat; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; visible: root.targets[target.currentIndex] === "dds"; textRole: "name"; valueRole: "code"; model: [{name: "Native", code: 0}, {name: "RGBA8 UNorm", code: 28}, {name: "RGBA32 Float", code: 2}, {name: "BC1 UNorm", code: 71}, {name: "BC3 UNorm", code: 77}, {name: "BC5 UNorm", code: 83}, {name: "BC5 SNorm", code: 84}, {name: "BC6H UF16", code: 95}, {name: "BC7 UNorm", code: 98}]; onActivated: root.edited(); Accessible.name: Theme.t("DDS 格式", "DDS format", "DDS 格式") }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: "Alpha" } Choice { id: alphaAction; objectName: "conversionAlphaAction"; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; model: [Theme.t("保留 Alpha", "Preserve alpha", "保留 Alpha"), Theme.t("删除 Alpha，保留 RGB", "Drop alpha, keep RGB", "移除 Alpha，保留 RGB"), Theme.t("在线性背景上合成", "Composite on linear background", "在線性背景上合成")]; onActivated: root.edited(); Accessible.name: Theme.t("Alpha 处理", "Alpha action", "Alpha 處理") } }
}
}
SectionHeader { background: Rectangle { color: Theme.raised; radius: Theme.groupRadius; border.color: Theme.line } id: advancedSettings; objectName: "conversionAdvancedSettings"; Layout.fillWidth: true; text: Theme.t("高级设置", "Advanced settings", "進階設定") }
ColumnLayout { Layout.fillWidth: true; visible: advancedSettings.checked; spacing: 20
FormGroup { objectName: "conversionContent"; title: Theme.t("内容选择", "Content selection", "內容選擇"); Layout.fillWidth: true
 ColumnLayout { anchors.fill: parent; spacing: 12
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("只读输入范围", "Input scope", "唯讀輸入範圍") } Field { id: input; objectName: "conversionInputRoot"; Layout.fillWidth: true; onTextEdited: root.edited(); Accessible.name: Theme.t("只读输入目录", "Read-only input directory", "唯讀輸入目錄") } Tool { iconName: "folder"; Accessible.name: Theme.t("选择输入目录", "Choose input folder", "選擇輸入資料夾"); onClicked: inputFolder.open() } }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("子资源索引", "Subresource index", "子資源索引") } NumberField { id: resource; Layout.preferredWidth: 160; Layout.maximumWidth: 180; Layout.fillWidth: true; from: 0; to: 65535; editable: true; onValueModified: root.edited(); Accessible.name: Theme.t("显式子资源索引", "Explicit subresource index", "明確子資源索引") } }
ConversionInputs { Layout.fillWidth: true; controller: root.controller; scope: ({input_roots: [input.text], output_roots: [output.text], allow_overwrite: overwrite.checked}); onAccepted: paths => { root.chooseInputs(paths); root.edited() } }
ConversionOverrides { id: perItem; Layout.fillWidth: true; inputs: root.inputs; inputInfo: root.controller.imageJobs.inputInfo; onEdited: root.edited() }
 Label { Layout.fillWidth: true; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("默认逐文件保留真实通道；不存在的通道或不兼容类型会在预检中阻止输出。", "Preserves each file's source channels. Absent channels and incompatible types block output in preflight.", "預設逐檔保留實際通道；不存在的通道或不相容類型會在預檢中阻止輸出。") }
 }
}
FormGroup { title: Theme.t("数值与元数据", "Numeric rules and metadata", "數值與中繼資料"); Layout.fillWidth: true
ColumnLayout { anchors.fill: parent; spacing: 12
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("源 Alpha", "Source alpha", "來源 Alpha") } Choice { id: sourceAlpha; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; model: [Theme.t("使用源 Alpha 标记", "Use source alpha tag", "使用來源 Alpha 標記"), Theme.t("无 Alpha", "No alpha", "無 Alpha"), Theme.t("直通", "Straight", "直通"), Theme.t("预乘", "Premultiplied", "預乘")]; onActivated: root.edited(); Accessible.name: Theme.t("源 Alpha 解释", "Source alpha interpretation", "來源 Alpha 解釋") } }
OptionCheck { Layout.fillWidth: true; id: colour; text: Theme.t("将输入明确解释为颜色", "Explicitly interpret input as colour", "將輸入明確解釋為色彩"); onToggled: root.edited() }
RowLayout { Layout.fillWidth: true; Layout.minimumHeight: 36; Label { Layout.preferredWidth: 118; font.pixelSize: Theme.compactText; color: Theme.muted; text: Theme.t("颜色变换", "Colour transfer", "色彩變換") } Choice { id: transfer; Layout.preferredWidth: 220; Layout.maximumWidth: 260; Layout.fillWidth: true; model: [Theme.t("不变换", "No transfer", "不變換"), "Linear → sRGB", "sRGB → Linear"]; enabled: colour.checked; onActivated: root.edited(); Accessible.name: Theme.t("颜色变换", "Colour transfer", "色彩變換") } }
OptionCheck { Layout.fillWidth: true; id: quantize; objectName: "conversionQuantize"; text: Theme.t("允许裁切至 [0,1] 并四舍五入量化（报告损失）", "Allow clamp to [0,1] and round-half-up quantization (report losses)", "允許裁切至 [0,1] 並四捨五入量化（報告損失）"); onToggled: root.edited() }
OptionCheck { Layout.fillWidth: true; id: replaceNonFinite; text: Theme.t("将 NaN / Inf 明确替换为 0（报告数量）", "Explicitly replace NaN / Inf with 0 (report counts)", "將 NaN / Inf 明確替換為 0（報告數量）"); onToggled: root.edited() }
OptionCheck { Layout.fillWidth: true; id: allowLossy; objectName: "conversionAllowLossy"; text: Theme.t("允许有损编码；仍须确认实际损失", "Allow lossy encoding; actual losses still require approval", "允許有損編碼；仍須確認實際損失"); onToggled: root.edited() }
SectionHeader { id: advanced; objectName: "conversionAdvanced"; Layout.fillWidth: true; text: Theme.t("曝光、窗口与元数据", "Exposure, window and metadata", "曝光、視窗與中繼資料") }
 ColumnLayout { Layout.fillWidth: true; visible: advanced.checked
RowLayout { Label { text: "EV"; color: Theme.text } ExposureSpinBox { id: exposure; enabled: colour.checked; onValueModified: root.edited(); Accessible.name: "EV" } Label { text: Theme.t("线性背景 %", "Linear background %", "線性背景 %"); color: Theme.text } NumberField { id: background; from: 0; to: 100; value: 18; editable: true; onValueModified: root.edited(); Accessible.name: Theme.t("线性背景百分比", "Linear background percent", "線性背景百分比") } }
OptionCheck { Layout.fillWidth: true; id: flatten; text: Theme.t("将选定 dataWindow 展平到原点（报告窗口损失）", "Flatten selected dataWindow to origin (report window loss)", "將選定 dataWindow 平面化至原點（報告視窗損失）"); onToggled: root.edited() }
OptionCheck { Layout.fillWidth: true; id: dropMetadata; objectName: "conversionMetadata"; text: Theme.t("明确丢弃元数据（报告损失）", "Explicitly drop metadata (report losses)", "明確捨棄中繼資料（報告損失）"); onToggled: root.edited() }
}
}
}
}
}
}

ScrollView {
    id: summaryScroll; Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 35; clip: true; contentWidth: availableWidth
FormGroup {
    objectName: "conversionSummary"; width: summaryScroll.availableWidth
    title: Theme.t("转换摘要", "Conversion summary", "轉換摘要")
    ColumnLayout { anchors.fill: parent; spacing: 16
        Readout { inlineValue: true; Layout.fillWidth: true; Layout.minimumHeight: 24; label: Theme.t("输入数量", "Input count", "輸入數量"); value: root.inputs.length + Theme.t(" 个输入", " inputs", " 個輸入") }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        Readout { inlineValue: true; Layout.fillWidth: true; label: Theme.t("输出格式", "Output format", "輸出格式"); value: target.currentText + " · " + depth.currentText }
        Readout { objectName: "conversionContentSource"; inlineValue: true; Layout.fillWidth: true; label: Theme.t("内容来源", "Content source", "內容來源"); value: Theme.t("源内容 · 不套用预览效果", "Source content · No preview effects", "來源內容 · 不套用預覽效果") }
        Readout { objectName: "conversionPreflightStatus"; inlineValue: true; Layout.fillWidth: true; label: Theme.t("预检状态", "Preflight status", "預檢狀態"); value: root.controller.imageJobs.busy ? Theme.t("正在准备…", "Preparing…", "正在準備…") : root.controller.imageJobs.error ? Theme.t("预检失败", "Preflight failed", "預檢失敗") : root.controller.imageJobs.plan.state ? Theme.state(root.controller.imageJobs.plan.state) : Theme.t("待预检", "Not preflighted", "待預檢") }
        Readout { inlineValue: true; Layout.fillWidth: true; label: Theme.t("Alpha 处理", "Alpha action", "Alpha 處理"); value: alphaAction.currentText }
        Readout { inlineValue: true; Layout.fillWidth: true; label: Theme.t("冲突策略", "Conflict policy", "衝突策略"); value: conflict.currentText }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        Readout { objectName: "conversionNamingExamples"; Layout.fillWidth: true; label: Theme.t("命名示例 · 预检前", "Name examples · Before preflight", "命名範例 · 預檢前"); value: root.namingPreview.error || (root.namingPreview.names || []).join("\n") || "—" }
        RowLayout { Layout.fillWidth: true; spacing: 10
            AppIcon { name: "preflight"; variant: "muted"; Layout.alignment: Qt.AlignTop }
            Label { Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("预检会检查输入、输出范围与格式兼容性，再确认转换。", "Preflight checks inputs, output scope and format compatibility before conversion.", "預檢會檢查輸入、輸出範圍與格式相容性，再確認轉換。") }
        }
    }
}
}
}
    RowLayout {
        objectName: "conversionFooter"; Layout.fillWidth: true; Layout.minimumHeight: 40
        Label { Layout.fillWidth: true; font.pixelSize: Theme.compactText; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("预检不写出文件；不套用视口曝光、背景或平铺。", "Preflight writes no files. Viewport exposure, background and tiling are excluded.", "預檢不寫出檔案；不套用視口曝光、背景或平鋪。") }
        Tool { text: Theme.t("取消", "Cancel", "取消"); onClicked: root.back() }
Tool { iconName: "preflight"; primary: true; objectName: "createImagePlan"; text: root.controller.imageJobs.busy ? Theme.t("正在准备…", "Preparing…", "正在準備…") : Theme.t("开始预检", "Start preflight", "開始預檢"); enabled: !root.controller.imageJobs.busy && root.inputs.length > 0 && !!input.text && !!output.text && (conflict.currentIndex !== 2 || overwrite.checked); onClicked: root.preflight({input_roots: [input.text], output_roots: [output.text], allow_overwrite: overwrite.checked}, root.options(), naming.text, ["skip", "rename", "overwrite"][conflict.currentIndex]) }
    }
FolderDialog { id: inputFolder; onAccepted: { input.text = root.controller.localFile(selectedFolder); root.edited() } }
FolderDialog { id: outputFolder; onAccepted: { output.text = root.controller.localFile(selectedFolder); root.edited() } }
}
