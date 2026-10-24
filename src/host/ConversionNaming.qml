pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
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
        Tool { form: true; objectName: "conversionNamingHelp"; text: Theme.t("命名规则 ⓘ", "Naming rules ⓘ", "命名規則 ⓘ"); onClicked: help.active = true }
    }
    RowLayout {
        Layout.fillWidth: true
        Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("命名模板", "Filename template", "命名範本") }
        Field { id: pattern; objectName: "conversionNamingTemplate"; form: true; Layout.fillWidth: true; text: "{name}-converted.{ext}"; Accessible.name: Theme.t("命名模板", "Filename template", "命名範本"); onTextEdited: { preset.currentIndex = root.templates.length - 1; root.edited() } }
    }
    Loader {
        id: help; active: false
        sourceComponent: Window {
            id: helpWindow; objectName: "conversionNamingWindow"; width: 480; height: 410; minimumWidth: 380; minimumHeight: 320
            visible: true; flags: Qt.Tool; modality: Qt.NonModal; color: Theme.base
            transientParent: root.Window.window
            title: Theme.t("命名规则", "Naming rules", "命名規則")
            onClosing: help.active = false
            ScrollView { id: helpScroll; anchors.fill: parent; anchors.margins: 24; contentWidth: availableWidth
                ColumnLayout { width: helpScroll.availableWidth; spacing: 16
                    Label { Layout.fillWidth: true; font.pixelSize: Theme.formHeading; color: Theme.text; text: helpWindow.title }
                    Label { Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("此窗口不会阻止主界面操作，可移动到旁边参照。", "This window stays nonmodal; move it aside while editing.", "此視窗不會阻止主介面操作，可移到旁邊參照。") }
                    Readout { form: true; Layout.fillWidth: true; label: "{name} / {index} / {ext}"; value: Theme.t("源文件名（不含扩展名） / 从 1 开始的输出序号 / 目标扩展名", "Source basename / Output index from 1 / Target extension", "來源檔名（不含副檔名） / 從 1 開始的輸出序號 / 目標副檔名") }
                    Readout { form: true; Layout.fillWidth: true; visible: root.modelMode; label: "{clip}"; value: Theme.t("单个源动作名；纯模型为 model。单文件包含多个动作时请用原名 + animations。", "Source clip name; model for model-only. Use name + animations for multiple clips in one file.", "單個來源動作名；純模型為 model。單檔包含多個動作時請用原名 + animations。") }
                    Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text; text: Theme.t("只允许文件名，不允许目录分隔符、未知变量和非法名称。重名按冲突策略处理；预检展示最终路径。", "Use filenames only: no separators, unknown tokens or invalid names. Preflight resolves collisions and shows final paths.", "只允許檔名，不允許目錄分隔符、未知變數和非法名稱。重名按衝突策略處理；預檢顯示最終路徑。") }
                    Tool { text: Theme.t("关闭", "Close", "關閉"); onClicked: help.active = false }
                }
            }
        }
    }
}
