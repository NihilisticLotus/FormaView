pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
ColumnLayout {
    id: root
    objectName: "modelExportSettings"
    required property var controller
    required property var inputs
    required property var scope
    signal edited()
    property var selectedClips: []
    property bool animationOnlyPreset: false
    property string inspectedKey: ""
    readonly property string inspectionKey: JSON.stringify([inputs, paired.text])
    function defaultContentIndex(): int { return inputs && inputs.length > 0 && String(inputs[0]).toLowerCase().endsWith(".psa") ? 2 : 1 }
    onInputsChanged: { selectedClips = []; content.currentIndex = animationOnlyPreset ? 2 : defaultContentIndex() }
    onAnimationOnlyPresetChanged: content.currentIndex = animationOnlyPreset ? 2 : defaultContentIndex()
    readonly property bool multipleStacks: organization.currentIndex === 1
    readonly property bool hasActorX: (inputs || []).some(p => /\.(psk|pskx|psa)$/i.test(String(p)))
    readonly property string contentLabel: content.displayText
    readonly property string coordinateLabel: axes.displayText + " · " + unit.displayText
    readonly property string contentValue: ["combined", "model", "animation"][content.currentIndex]
    readonly property var inspectedSources: inspectedKey === inspectionKey ? controller.imageJobs.modelSources || [] : []
    readonly property var sourceClips: collectClips(inspectedSources)
    function collectClips(sources): var {
        const result = [], seen = []
        for (const source of sources) {
            for (const clip of source.clips || []) {
                if (!seen.includes(clip.id)) {
                    seen.push(clip.id)
                    result.push({id: clip.id, name: clip.name, start: clip.start, end: clip.end, input: source.animation_path})
                }
            }
        }
        return result
    }
    function options(): var {
        const result = {target: "fbx", coordinates: "actorx-z-up-x-forward-rh-cm", export_content: contentValue,
            file_organization: multipleStacks ? "multiple-stacks" : "per-clip", paired_file: paired.text,
            pairing_policy: pairing.currentIndex === 0 ? "strict-records" : "reference-skeleton",
            output_axes: ["source", "maya", "houdini", "unreal", "3ds-max", "cinema4d", "blender", "unity"][axes.currentIndex],
            output_unit: ["source", "cm", "m", "mm", "in"][unit.currentIndex]}
        if (clipMode.currentIndex === 1) result.selected_clips = selectedClips
        return result
    }
    FormGroup {
        title: Theme.t("模型与动画", "Model and animation", "模型與動畫"); Layout.fillWidth: true
        ColumnLayout { anchors.fill: parent; spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("导出内容", "Export content", "匯出內容") }
                Choice { id: content; form: true; objectName: "modelExportContent"; Layout.fillWidth: true
                    labels: [Theme.t("模型 + 动画", "Model + animation", "模型 + 動畫"), Theme.t("只导出模型", "Model only", "只匯出模型"), Theme.t("只导出动画", "Animation only", "只匯出動畫")]
                    notes: [Theme.t("写出模型与选中动画。", "Export the model and selected animations.", "寫出模型與選取動畫。"), Theme.t("只写模型、骨架与绑定，不创建动画。", "Export geometry, skeleton and bind pose, without animation.", "只寫模型、骨架與繫結，不建立動畫。"), Theme.t("只写必要骨架与动画；参考模型仍可用于解释骨架。", "Export skeleton and animation; a reference model can still supply the skeleton.", "只寫必要骨架與動畫；參考模型仍可用於解釋骨架。")]
                    onActivated: root.edited(); Accessible.name: Theme.t("导出内容", "Export content", "匯出內容")
                }
            }
            RowLayout { Layout.fillWidth: true; visible: root.hasActorX && content.currentIndex !== 1
                Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("配对文件", "Paired file", "配對檔案") }
                Field { id: paired; objectName: "modelExportPaired"; form: true; Layout.fillWidth: true; placeholderText: Theme.t("模型配 PSA；PSA 配参考 PSK / PSKX", "PSA for model; reference PSK/PSKX for PSA", "模型配 PSA；PSA 配參考 PSK / PSKX"); onTextEdited: { root.selectedClips = []; root.edited() } Accessible.name: placeholderText }
                Tool { form: true; iconName: "folder"; hint: Theme.t("选择配对文件", "Choose paired file", "選擇配對檔案"); onClicked: pairFile.open() }
            }
            RowLayout { Layout.fillWidth: true; visible: root.hasActorX && content.currentIndex !== 1
                Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("骨架配对", "Skeleton pairing", "骨架配對") }
                Choice { id: pairing; objectName: "modelExportPairing"; currentIndex: 1; form: true; Layout.fillWidth: true
                    labels: [Theme.t("严格匹配源记录", "Strict source records", "嚴格匹配來源記錄"), Theme.t("使用参考模型骨架", "Use reference skeleton", "使用參考模型骨架")]
                    notes: [Theme.t("同时核对 PSA 与 PSK 的父子关系和参考姿态；占位记录会被拒绝。", "Also require matching PSA/PSK hierarchy and reference pose; placeholder records are rejected.", "同時核對 PSA 與 PSK 的父子關係和參考姿態；佔位記錄會被拒絕。"), Theme.t("与预览一致：按唯一骨骼名配对，采用 PSK / PSKX 的层级与参考姿态。", "Same as preview: match unique bone names and use the PSK/PSKX hierarchy and reference pose.", "與預覽一致：按唯一骨骼名配對，採用 PSK / PSKX 的階層與參考姿態。")]
                    onActivated: root.edited(); Accessible.name: Theme.t("骨架配对", "Skeleton pairing", "骨架配對")
                }
            }
            Label { Layout.fillWidth: true; visible: root.hasActorX && content.currentIndex !== 1; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.WordWrap; text: pairing.notes[pairing.currentIndex] }
            Readout { form: true; Layout.fillWidth: true; visible: root.hasActorX && content.currentIndex !== 1; label: Theme.t("PSA 配置 · 自动适配", "PSA config · Automatic", "PSA 設定 · 自動適配"); value: Theme.t("自动查找同目录同名 .config；预检列明采用路径、RemoveTracks 与参考恢复规则。", "Find same-directory, same-basename .config; preflight lists the applied file, RemoveTracks and reference recovery rules.", "自動尋找同目錄同名 .config；預檢列明採用路徑、RemoveTracks 與參考恢復規則。") }
            RowLayout { Layout.fillWidth: true; visible: content.currentIndex !== 1
                Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("动作片段", "Animation clips", "動作片段") }
                Choice { id: clipMode; objectName: "modelExportClipMode"; form: true; Layout.fillWidth: true
                    labels: [Theme.t("全部源片段", "All source clips", "全部來源片段"), Theme.t("选定片段", "Selected clips", "選取片段")]
                    notes: [Theme.t("预检从真实源文件展开全部片段。", "Preflight expands all clips from source files.", "預檢從實際來源檔案展開全部片段。"), Theme.t("先读取列表，再勾选需要的源动作。", "Read the list, then select source clips.", "先讀取清單，再勾選需要的來源動作。")]
                    onActivated: root.edited(); Accessible.name: Theme.t("动作选择", "Clip selection", "動作選擇")
                }
                Tool { form: true; objectName: "modelExportInspect"; text: Theme.t("读取列表", "Read clips", "讀取清單"); enabled: !root.controller.imageJobs.busy && !!root.inputs && root.inputs.length > 0 && !!root.scope?.input_roots?.length && root.scope.input_roots.every(p => typeof p === "string" && !!p.trim()); onClicked: { root.inspectedKey = root.inspectionKey; root.controller.imageJobs.inspectModelInputs(root.inputs, root.scope, paired.text) } }
            }
            ColumnLayout { Layout.fillWidth: true; visible: content.currentIndex !== 1 && (clipMode.currentIndex === 1 || root.sourceClips.length > 0)
                Repeater { model: root.inspectedSources
                    ColumnLayout { required property var modelData; visible: !!modelData.error; Layout.fillWidth: true; spacing: 2
                        Label { Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; font.pixelSize: Theme.noteText; text: parent.modelData.input_path || "" }
                        ExplainedText { Layout.fillWidth: true; kind: "error"; raw: parent.modelData.error ? parent.modelData.error.code + ": " + parent.modelData.error.message : "" }
                    }
                }
                Label { visible: root.sourceClips.length === 0; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("读取源文件后显示动作；未选片段不能导出。", "Read source clips first; select at least one clip to export.", "讀取來源檔案後顯示動作；未選片段不能匯出。") }
                Repeater { model: root.sourceClips
                    OptionCheck { required property var modelData; objectName: "modelExportClip"; Layout.fillWidth: true; enabled: clipMode.currentIndex === 1; text: modelData.name + " · " + Number(modelData.start).toFixed(3) + "–" + Number(modelData.end).toFixed(3) + " s"; checked: clipMode.currentIndex === 0 || root.selectedClips.includes(modelData.id); ToolTip.visible: hovered; ToolTip.text: modelData.input; onToggled: { root.selectedClips = checked ? root.selectedClips.concat([modelData.id]) : root.selectedClips.filter(id => id !== modelData.id); root.edited() } }
                }
            }
            RowLayout { Layout.fillWidth: true; visible: content.currentIndex !== 1
                Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("文件组织", "File organization", "檔案組織") }
                Choice { id: organization; objectName: "modelExportOrganization"; form: true; Layout.fillWidth: true
                    labels: [Theme.t("每片段一个 FBX", "One FBX per clip", "每片段一個 FBX"), Theme.t("一个 FBX · 多个独立动作", "One FBX · Independent clips", "一個 FBX · 多個獨立動作")]
                    notes: [Theme.t("每个源动作独立文件，保留各自时长。", "Each source clip becomes a separate file with its original duration.", "每個來源動作獨立檔案，保留各自時長。"), Theme.t("同一兼容模型的动作各占一个 Stack / Take；不串接时间轴，不混为动画层。", "Clips of one compatible model each have an independent Stack/Take; timelines are not concatenated or mixed as layers.", "同一相容模型的動作各佔一個 Stack / Take；不串接時間軸，不混為動畫層。")]
                    onActivated: root.edited(); Accessible.name: Theme.t("文件组织", "File organization", "檔案組織")
                }
            }
            Label { Layout.fillWidth: true; font.pixelSize: Theme.noteText; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("保留源根运动与真实时长。视口根锁、当前帧和显示骨骼不改变导出。", "Preserve source root motion and duration. Viewport root lock, current frame and bone display do not affect export.", "保留來源根運動與實際時長。視口根鎖、目前影格和顯示骨骼不改變匯出。") }
        }
    }
    FormGroup { title: Theme.t("导出坐标", "Output coordinates", "匯出座標"); Layout.fillWidth: true
        ColumnLayout { anchors.fill: parent; spacing: 12
            RowLayout { Layout.fillWidth: true
                Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("坐标预设", "Axis preset", "座標預設") }
                Choice { id: axes; objectName: "modelExportAxes"; form: true; Layout.fillWidth: true
                    labels: [Theme.t("保留源坐标", "Preserve source", "保留來源座標"), "Maya · Y↑ / RH", "Houdini · Y↑ / RH", "Unreal Engine · Z↑ / LH", "3ds Max · Z↑ / RH", "Cinema 4D · Y↑ / LH", "Blender · Z↑ / RH", "Unity · Y↑ / LH"]
                    notes: [Theme.t("FBX 保留文件声明；glTF/VRM 为右手 Y 上、米；ActorX 为 Z 上、X 前、右手、厘米。OBJ/PMX 的源单位假设列入预检。", "FBX uses file metadata; glTF/VRM use right-handed Y-up metres; ActorX uses right-handed Z-up/X-forward cm. Preflight lists OBJ/PMX unit assumptions.", "FBX 保留檔案宣告；glTF/VRM 為右手 Y 上、公尺；ActorX 為 Z 上、X 前、右手、公分。OBJ/PMX 的來源單位假設列入預檢。"), Theme.t("Maya 默认 Y 上、厘米；单位仍由下方单独设置。", "Maya defaults to Y up and cm; set output units separately below.", "Maya 預設 Y 上、公分；單位仍由下方單獨設定。"), Theme.t("Houdini 常用米；选择预设不会自动改变输出单位。", "Houdini commonly uses metres; this preset does not change output units.", "Houdini 常用公尺；選擇預設不會自動改變輸出單位。"), Theme.t("左手；+X 前，+Y 右，+Z 上；常用厘米。", "Left-handed: +X forward, +Y right, +Z up; commonly cm.", "左手；+X 前，+Y 右，+Z 上；常用公分。"), Theme.t("右手、Z 上；System Unit 默认英寸，与显示单位不同。", "Right-handed Z up; System Unit defaults to inches and differs from display units.", "右手、Z 上；System Unit 預設英吋，與顯示單位不同。"), Theme.t("左手、Y 上；按实际 Project Scale 选择单位。", "Left-handed Y up; use the actual Project Scale.", "左手、Y 上；按實際 Project Scale 選擇單位。"), Theme.t("右手、Z 上；按 Scene Units 选择单位，不猜测角色朝向。", "Right-handed Z up; use Scene Units. Character facing is not inferred.", "右手、Z 上；按 Scene Units 選擇單位，不猜測角色朝向。"), Theme.t("左手；+Z 前，+X 右，+Y 上；常用米。", "Left-handed: +Z forward, +X right, +Y up; commonly metres.", "左手；+Z 前，+X 右，+Y 上；常用公尺。")]
                    onActivated: root.edited(); Accessible.name: Theme.t("导出坐标预设", "Output axis preset", "匯出座標預設")
                }
            }
            RowLayout { Layout.fillWidth: true
                Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("输出单位", "Output units", "輸出單位") }
                Choice { id: unit; objectName: "modelExportUnit"; form: true; Layout.fillWidth: true; labels: [Theme.t("保留源单位", "Preserve source units", "保留來源單位"), "cm", "m", "mm", "in"]
                    notes: [Theme.t("按源格式保留单位；无单位声明时在预检中说明假设。", "Preserve source units; preflight reports assumptions when units are not declared.", "按來源格式保留單位；無單位宣告時在預檢中說明假設。"), "1 cm = 0.01 m", "1 m = 100 cm", "1 mm = 0.1 cm", "1 in = 2.54 cm"]
                    onActivated: root.edited(); Accessible.name: Theme.t("输出单位", "Output units", "輸出單位")
                }
            }
            Label { Layout.fillWidth: true; font.pixelSize: Theme.noteText; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("统一转换几何、法线、骨架、绑定和所有动作，保持实际尺寸。RH = 右手，LH = 左手。", "Convert geometry, normals, skeleton, bind poses and all clips together, preserving physical size. RH = right-handed, LH = left-handed.", "統一轉換幾何、法線、骨架、繫結和所有動作，保持實際尺寸。RH = 右手，LH = 左手。") }
        }
    }
    FileDialog { id: pairFile; nameFilters: ["ActorX (*.psk *.pskx *.psa)"]; onAccepted: { paired.text = root.controller.localFile(selectedFile); root.selectedClips = []; root.edited() } }
}
