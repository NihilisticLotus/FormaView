pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
    objectName: "conversionInputs"
    required property var controller
    required property var scope
    readonly property var service: controller.imageJobs
    property var selected: []
    property bool preparing: false
    property string pendingDirectory: ""
    property bool pendingRecursive: false
    property string snapshotId: ""
    readonly property var visibleItems: (service.discovery.items || []).filter(v => {
        const name = v.input.replace(/\\/g, "/").split("/").pop().toLowerCase()
        return name.includes(filter.text.toLowerCase()) && (extension.currentIndex === 0 || name.endsWith(extension.currentText.toLowerCase()))
    })
    signal accepted(var paths)
    function toggle(path, checked): void {
        if (checked && selected.length < 64 && !selected.includes(path)) selected = selected.concat([path])
        else if (!checked) selected = selected.filter(v => v !== path)
    }
    Connections {
        target: root.service
        function onChanged() {
            if (root.preparing && !root.service.busy) {
                root.preparing = false
                if (!root.service.error && root.service.configured)
                    root.service.discover(root.pendingDirectory, root.pendingRecursive)
            }
            const id = root.service.discovery.id || ""
            if (id !== root.snapshotId) { root.snapshotId = id; root.selected = [] }
        }
    }
    Tool { id: expand; objectName: "discoverInputsToggle"; checkable: true; text: (checked ? "▾ " : "▸ ") + Theme.t("从输入目录选择图片", "Choose images from input folder", "從輸入資料夾選擇影像") }
    ColumnLayout {
        Layout.fillWidth: true; visible: expand.checked
        RowLayout {
            CheckBox { id: recursive; objectName: "discoverRecursive"; text: Theme.t("包含子目录", "Include subfolders", "包含子資料夾") }
            Tool {
                objectName: "discoverImagesButton"; text: root.service.busy || root.preparing ? Theme.t("读取中…", "Reading…", "讀取中…") : Theme.t("读取目录列表", "Read folder list", "讀取資料夾清單")
                enabled: !root.service.busy && !root.preparing && !!root.scope.input_roots[0] && !!root.scope.output_roots[0]
                onClicked: { root.pendingDirectory = root.scope.input_roots[0]; root.pendingRecursive = recursive.checked; root.service.configureScope(root.scope); root.preparing = root.service.busy }
            }
        }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("列表只固定目录成员；预检会重新读取并校验内容。重解析点不跟随。每次最多选择 64 项。", "The list fixes membership only; preflight rereads and validates content. Reparse points are not followed. Select up to 64 items.", "清單只固定資料夾成員；預檢會重新讀取並驗證內容。不跟隨重新解析點。每次最多選擇 64 項。") }
        Readout { Layout.fillWidth: true; visible: !!root.snapshotId; label: Theme.t("本次目录列表", "Current folder list", "本次資料夾清單"); value: (root.service.discovery.directory || "") + "\n" + (root.service.discovery.total || 0) + Theme.t(" 项 · 已选 ", " items · Selected ", " 項 · 已選 ") + root.selected.length }
        RowLayout {
            visible: !!root.snapshotId; Layout.fillWidth: true
            Field { id: filter; objectName: "discoveryFilter"; Layout.fillWidth: true; placeholderText: Theme.t("筛选本页文件名", "Filter names on this page", "篩選本頁檔名"); Accessible.name: placeholderText }
            ComboBox { id: extension; Layout.preferredWidth: 210; model: [Theme.t("本页全部格式", "All formats on page", "本頁全部格式"), ".png", ".jpg", ".jpeg", ".bmp", ".dib", ".gif", ".ico", ".tga", ".tif", ".tiff", ".webp", ".dds", ".hdr", ".exr"]; Accessible.name: Theme.t("本页扩展名筛选", "Page extension filter", "本頁副檔名篩選") }
        }
        ListView {
            objectName: "discoveryList"; Layout.fillWidth: true; Layout.preferredHeight: Math.min(240, count * 40); clip: true
            model: root.visibleItems; reuseItems: true
            ScrollBar.vertical: ScrollBar {}
            delegate: CheckBox {
                objectName: "discoveryChoice"
                required property var modelData
                width: ListView.view.width; height: 40
                text: modelData.input.replace(/\\/g, "/").split("/").pop()
                checked: root.selected.includes(modelData.input)
                enabled: checked || root.selected.length < 64
                Accessible.name: modelData.input
                ToolTip.visible: hovered; ToolTip.text: modelData.input
                onToggled: root.toggle(modelData.input, checked)
            }
        }
        RowLayout {
            visible: !!root.snapshotId; Layout.fillWidth: true
            Tool { objectName: "discoveryPrevious"; text: "‹"; enabled: root.service.discoveryOffset > 0; Accessible.name: Theme.t("上一页输入", "Previous input page", "上一頁輸入"); onClicked: root.service.discoveryPage(root.service.discoveryOffset - 100) }
            Label { textFormat: Text.PlainText; color: Theme.muted; text: (root.service.discoveryOffset + 1) + " / " + (root.service.discovery.total || 0) }
            Tool { objectName: "discoveryNext"; text: "›"; enabled: root.service.discovery.next_offset !== null && root.service.discovery.next_offset !== undefined; Accessible.name: Theme.t("下一页输入", "Next input page", "下一頁輸入"); onClicked: root.service.discoveryPage(root.service.discoveryOffset + 100) }
            Tool { objectName: "selectDiscoveredPage"; text: Theme.t("选择本页筛选项", "Select filtered page", "選取本頁篩選項"); enabled: root.visibleItems.length > 0 && root.selected.length < 64; onClicked: { for (const item of root.visibleItems) root.toggle(item.input, true) } }
            Tool { text: Theme.t("清空选择", "Clear selection", "清空選擇"); onClicked: root.selected = [] }
        }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; visible: (root.service.discovery.rejected_reparse_points || []).length > 0; text: Theme.t("已跳过重解析点：", "Skipped reparse points: ", "已略過重新解析點：") + (root.service.discovery.rejected_reparse_points || []).length }
        Tool { objectName: "useDiscoveredInputs"; enabled: root.selected.length > 0; text: Theme.t("将选择作为固定输入列表", "Use selection as fixed inputs", "將選擇設為固定輸入清單"); onClicked: { root.accepted(root.selected.slice()); expand.checked = false } }
    }
}
