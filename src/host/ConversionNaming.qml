pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
    required property var controller
    property bool modelMode: false
    property bool multipleStacks: false
    property alias nameTemplate: pattern.text
    signal edited()
    onVisibleChanged: if (!visible) help.active = false
    readonly property var templates: modelMode ? ["{name}.{ext}", "{name}-converted.{ext}", "{name}-{clip}.{ext}", "{name}-animations.{ext}", "{index}-{name}.{ext}", ""] : ["{name}.{ext}", "{name}-converted.{ext}", "{index}-{name}.{ext}", ""]
    function chooseDefault(): void {
        preset.currentIndex = modelMode ? (multipleStacks ? 3 : 2) : 1
        pattern.text = modelMode ? (multipleStacks ? "{name}-animations.{ext}" : "{name}-{clip}.{ext}") : "{name}-converted.{ext}"
    }
    onModelModeChanged: chooseDefault()
    onMultipleStacksChanged: {
        if (modelMode && multipleStacks && pattern.text === "{name}-{clip}.{ext}") { preset.currentIndex = 3; pattern.text = "{name}-animations.{ext}"; edited() }
        else if (modelMode && !multipleStacks && pattern.text === "{name}-animations.{ext}") { preset.currentIndex = 2; pattern.text = "{name}-{clip}.{ext}"; edited() }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 12
        Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("命名预设", "Naming preset", "命名預設") }
        Choice {
            id: preset; objectName: "conversionNamingPreset"; form: true; Layout.fillWidth: true; currentIndex: 1
            labels: root.modelMode ? [Theme.t("保留原名", "Source name", "保留原名"), Theme.t("原名 + converted", "Name + converted", "原名 + converted"), Theme.t("原名 + 动作片段", "Name + clip", "原名 + 動作片段"), Theme.t("原名 + animations", "Name + animations", "原名 + animations"), Theme.t("序号 + 原名", "Index + name", "序號 + 原名"), Theme.t("自定义", "Custom", "自訂")] : [Theme.t("保留原名", "Source name", "保留原名"), Theme.t("原名 + converted", "Name + converted", "原名 + converted"), Theme.t("序号 + 原名", "Index + name", "序號 + 原名"), Theme.t("自定义", "Custom", "自訂")]
            notes: root.templates.map(t => t || Theme.t("编辑下方模板；规则小窗可随时查看。", "Edit the template below; naming help stays available.", "編輯下方範本；規則小窗可隨時查看。"))
            onActivated: { if (root.templates[currentIndex]) pattern.text = root.templates[currentIndex]; root.edited() }
            Accessible.name: Theme.t("命名预设", "Naming preset", "命名預設")
        }
        Tool { form: true; objectName: "conversionNamingHelp"; text: Theme.t("命名规则 ⓘ", "Naming rules ⓘ", "命名規則 ⓘ"); onClicked: { help.active = true; if (help.item) { help.item.raise(); help.item.requestActivate() } } }
    }
    RowLayout {
        Layout.fillWidth: true
        Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("命名模板", "Filename template", "命名範本") }
        Field { id: pattern; objectName: "conversionNamingTemplate"; form: true; Layout.fillWidth: true; text: "{name}-converted.{ext}"; Accessible.name: Theme.t("命名模板", "Filename template", "命名範本"); onTextEdited: { preset.currentIndex = root.templates.length - 1; root.edited() } }
    }
    Loader {
        id: help; active: false
        sourceComponent: AppToolWindow {
            id: helpWindow; controller: root.controller; objectName: "conversionNamingWindow"; width: 580; height: root.modelMode ? 560 : 480; minimumWidth: 440; minimumHeight: 340
            visible: true
            transientParent: root.Window.window
            title: Theme.t("命名规则", "Naming rules", "命名規則")
            onClosing: help.active = false
            ScrollView { id: helpScroll; anchors.fill: parent; anchors.margins: 24; contentWidth: availableWidth
                ColumnLayout { width: helpScroll.availableWidth; spacing: 16
                    Label { Layout.fillWidth: true; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("可移动到旁边参照，同时继续编辑转换设置。", "Move this window aside while editing conversion settings.", "可移到旁邊參照，同時繼續編輯轉換設定。") }
                    Readout { form: true; Layout.fillWidth: true; label: "{name}"; value: Theme.t("源文件名，不含扩展名。例如 T_Ring_066。", "Source filename without its extension, such as T_Ring_066.", "來源檔名，不含副檔名。例如 T_Ring_066。") }
                    Readout { form: true; Layout.fillWidth: true; label: "{index}"; value: Theme.t("从 1 开始的输出序号，自动分批时连续编号。", "Output index starting at 1, continuous across automatic batches.", "從 1 開始的輸出序號，自動分批時連續編號。") }
                    Readout { form: true; Layout.fillWidth: true; label: "{ext}"; value: root.modelMode ? Theme.t("目标格式扩展名：fbx。", "Target extension: fbx.", "目標格式副檔名：fbx。") : Theme.t("目标格式扩展名，例如 tga 或 png。", "Target extension, such as tga or png.", "目標格式副檔名，例如 tga 或 png。") }
                    Readout { form: true; Layout.fillWidth: true; visible: root.modelMode; label: "{clip}"; value: Theme.t("单个源动作名；纯模型为 model。\n多个动作同文件时用 {name}-animations.{ext}。", "Source clip name; model for model-only.\nMultiple clips in one file: {name}-animations.{ext}.", "單個來源動作名；純模型為 model。\n多個動作同檔時用 {name}-animations.{ext}。") }
                    Readout { form: true; Layout.fillWidth: true; label: Theme.t("示例", "Example", "範例"); value: root.modelMode ? "{name}-{clip}.{ext}\nCharacter-Walk.fbx" : "{name}-converted.{ext}\nT_Ring_066-converted.tga" }
                    Label { Layout.fillWidth: true; font.pixelSize: Theme.noteText; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("只允许文件名，不允许目录分隔符、未知变量和非法名称。重名按冲突策略处理；预检展示最终路径。", "Use filenames only: no separators, unknown tokens or invalid names. Preflight resolves collisions and shows final paths.", "只允許檔名，不允許目錄分隔符、未知變數和非法名稱。重名按衝突策略處理；預檢顯示最終路徑。") }
                    Tool { form: true; Layout.alignment: Qt.AlignRight; text: Theme.t("关闭", "Close", "關閉"); onClicked: helpWindow.close() }
                }
            }
        }
    }
}
