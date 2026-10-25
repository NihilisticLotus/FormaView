pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
ColumnLayout {
    id: root
    required property var controller
    readonly property var service: controller.imageJobs
    Layout.fillWidth: true
    spacing: 10
    RowLayout {
        Layout.fillWidth: true; spacing: 12
        Label { Layout.preferredWidth: 148; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("输入图片", "Input images", "輸入影像") }
        Label { objectName: "conversionInputCount"; Layout.fillWidth: true; font.pixelSize: Theme.formText; color: Theme.text; text: Theme.t("已添加 %1 张", "%1 images added", "已加入 %1 張").arg(root.service.sourcePaths.length) }
        Tool { objectName: "addConversionImages"; form: true; text: Theme.t("添加图片…", "Add images…", "加入影像…"); enabled: !root.service.importing; onClicked: files.open() }
        Tool { objectName: "addConversionFolder"; form: true; iconName: "folder"; text: Theme.t("添加文件夹…", "Add folder…", "加入資料夾…"); enabled: !root.service.importing; onClicked: folder.open() }
    }
    RowLayout {
        Layout.fillWidth: true
        OptionCheck { objectName: "conversionIncludeSubfolders"; id: recursive; font.pixelSize: Theme.formText; text: Theme.t("包含子文件夹", "Include subfolders", "包含子資料夾") }
        Item { Layout.fillWidth: true }
        Tool { objectName: "conversionInputList"; id: expanded; form: true; quiet: true; checkable: true; text: Theme.t("查看列表", "View list", "檢視清單"); iconName: "expand"; iconRotation: checked ? 180 : 0 }
        Tool { objectName: "clearConversionInputs"; form: true; quiet: true; text: Theme.t("清空", "Clear", "清空"); enabled: root.service.sourcePaths.length > 0 || root.service.importing; onClicked: root.service.clearSources() }
    }
    RowLayout {
        visible: root.service.importing; Layout.fillWidth: true
        Label { Layout.fillWidth: true; font.pixelSize: Theme.formText; color: Theme.muted; text: Theme.t("正在读取图片列表…", "Reading image list…", "正在讀取影像清單…") }
        Tool { objectName: "cancelConversionImport"; form: true; text: Theme.t("取消读取", "Cancel reading", "取消讀取"); onClicked: root.service.cancelImport() }
    }
    ExplainedText { objectName: "conversionImportError"; Layout.fillWidth: true; kind: "error"; raw: root.service.importResult.error ? (root.service.importResult.error.message || root.service.importResult.error) : "" }
    Label {
        objectName: "conversionImportSummary"; Layout.fillWidth: true; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.WordWrap
        visible: root.service.importResult.added !== undefined
        text: root.service.importResult.empty ? Theme.t("未找到支持的图片。", "No supported images found.", "未找到支援的影像。") :
            Theme.t("新增 %1 张 · 重复 %2 张 · 忽略 %3 项 · 跳过不安全路径 %4 项", "Added %1 · Duplicates %2 · Ignored %3 · Unsafe paths skipped %4", "新增 %1 張 · 重複 %2 張 · 忽略 %3 項 · 略過不安全路徑 %4 項")
                .arg(root.service.importResult.added || 0).arg(root.service.importResult.duplicates || 0).arg(root.service.importResult.ignored || 0).arg(root.service.importResult.rejected || 0)
    }
    Readout { objectName: "conversionRejectedPaths"; form: true; Layout.fillWidth: true; visible: (root.service.importResult.rejected_paths || []).length > 0; label: Theme.t("已跳过不安全路径", "Skipped unsafe paths", "已略過不安全路徑"); value: (root.service.importResult.rejected_paths || []).map(v => v.path + "\n" + Explain.errorText(v.reason)).join("\n") }
    ListView {
        objectName: "conversionSelectedInputs"; visible: expanded.checked; Layout.fillWidth: true
        Layout.preferredHeight: Math.min(240, count * 44); clip: true; reuseItems: true
        model: root.service.sourcePaths; ScrollBar.vertical: ScrollBar {}
        delegate: RowLayout {
            required property string modelData
            width: ListView.view.width; height: 44; spacing: 8
            Label { Layout.fillWidth: true; font.pixelSize: Theme.formText; color: Theme.text; text: parent.modelData; elide: Text.ElideMiddle; textFormat: Text.PlainText; ToolTip.visible: hover.hovered; ToolTip.text: text; HoverHandler { id: hover } }
            Tool { form: true; text: Theme.t("移除", "Remove", "移除"); Accessible.name: text + " " + parent.modelData; onClicked: root.service.removeSource(parent.modelData) }
        }
    }
    Readout { objectName: "conversionSourceRoots"; form: true; Layout.fillWidth: true; label: Theme.t("来源目录", "Source folders", "來源資料夾"); value: root.service.sourceRoots.join("\n") || "—" }
    Label { Layout.fillWidth: true; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("继续添加会追加并去重；文件夹默认只读取当前层。超过 64 张自动分批，最多 4096 张。", "Additions are appended and deduplicated. Folders include this level by default. More than 64 images are split into batches; maximum 4096.", "繼續加入會追加並去重；資料夾預設只讀取目前層。超過 64 張自動分批，最多 4096 張。") }
    FileDialog {
        id: files; objectName: "conversionImageFileDialog"; fileMode: FileDialog.OpenFiles
        title: Theme.t("添加转换图片", "Add conversion images", "加入轉換影像")
        nameFilters: [Theme.t("支持的图片", "Supported images", "支援的影像") + " (" + (root.service.capabilities.read_extensions || []).map(e => "*" + e).join(" ") + ")"]
        onAccepted: root.service.addSourceFiles(selectedFiles.map(f => root.controller.localFile(f)))
    }
    FolderDialog { id: folder; title: Theme.t("添加图片文件夹", "Add image folder", "加入影像資料夾"); onAccepted: root.service.addSourceFolder(root.controller.localFile(selectedFolder), recursive.checked) }
}
