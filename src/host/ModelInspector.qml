pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQml.Models

Item {
    id: root
    objectName: "modelInspector"
    required property var controller
    readonly property var session: controller.model
    property int tab: 0
    onTabChanged: {
        if (tab!==1 && session.mode===2) session.setMode(0)
    }
    ColumnLayout {
        anchors.fill: parent; spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: [Theme.t("场景", "Scene", "場景"),Theme.t("骨骼", "Skeleton", "骨骼"),Theme.t("材质", "Materials", "材質")]
                delegate: PageTab {
                    required property int index
                    required property string modelData
                    objectName: "modelInspectorTab" + index
                    Layout.fillWidth: true
                    text: modelData; checked: root.tab===index
                    onClicked: root.tab=index
                }
            }
        }
        ScrollView {
            id: sceneScroll
            objectName: "modelSceneScroll"
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: root.tab===0
            contentWidth: availableWidth
            ColumnLayout {
                objectName: "modelSceneColumn"
                width: sceneScroll.availableWidth; spacing: 10
                Label { text: Theme.t("源场景 · 只读", "Source scene · Read only", "來源場景 · 唯讀"); color: Theme.text; font.bold: true }
                TreeView {
                    id: tree
                    objectName: "modelSceneTree"
                    Layout.fillWidth: true; Layout.preferredHeight: 330
                    model: root.session.scene
                    clip: true
                    editTriggers: TableView.NoEditTriggers
                    selectionModel: ItemSelectionModel { model: root.session.scene }
                    columnWidthProvider: () => tree.width
                    function revealSelection(): void {
                        if (!root.session.available) return
                        const selected = root.session.treeSelection()
                        if (selected.valid) tree.expandToIndex(selected)
                    }
                    Component.onCompleted: Qt.callLater(tree.revealSelection)
                    Connections { target: root.session; function onDocumentChanged() { Qt.callLater(tree.revealSelection) } }
                    delegate: TreeViewDelegate {
                        id: node
                        required property int column
                        required property string sourceLabel
                        required property int meshIndex
                        required property bool bone
                        implicitWidth: tree.width; implicitHeight: Theme.rowHeight
                        text: sourceLabel
                        contentItem: Label { text: node.text; color: node.meshIndex===root.session.meshIndex ? Theme.text : Theme.muted; elide: Text.ElideMiddle; textFormat: Text.PlainText; verticalAlignment: Text.AlignVCenter; font.pixelSize: Theme.compactText }
                        background: Rectangle { color: node.meshIndex===root.session.meshIndex ? Theme.selection : node.hovered ? Theme.hover : "transparent"; radius: Theme.radius; border.color: node.activeFocus ? Theme.accent : "transparent"; border.width: 2 }
                        onClicked: root.session.selectTreeIndex(tree.index(row,column))
                        Accessible.name: node.sourceLabel
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
                Choice { Layout.fillWidth: true; model: root.session.meshes; currentIndex: root.session.meshIndex; onActivated: root.session.selectMesh(currentIndex); Accessible.name: Theme.t("检查网格", "Inspected mesh", "檢查網格") }
                OptionCheck { text: Theme.t("仅显示当前网格", "Isolate this mesh", "僅顯示目前網格"); checked: root.session.isolated; onToggled: root.session.setIsolated(checked) }
                SectionHeader { id: sourceNode; objectName: "modelNodeMatrices"; Layout.fillWidth: true; text: Theme.t("节点与默认矩阵", "Node and default matrices", "節點與預設矩陣") }
                Loader { objectName: "modelNodeDetails"; Layout.fillWidth: true; active: sourceNode.checked; visible: active; sourceComponent: Component { ModelSourceDetails { source: root.session.sourceNode } } }
                Loader {
                    objectName: "modelLodDetails"
                    Layout.fillWidth: true; active: root.session.mode===3; visible: active
                    sourceComponent: Component { ModelLodPanel { session: root.session } }
                }
                SectionHeader { id: statistics; objectName: "modelStatistics"; Layout.fillWidth: true; text: Theme.t("统计与来源", "Statistics and source", "統計與來源") }
                ColumnLayout {
                    Layout.fillWidth: true; visible: statistics.checked
                    Readout { Layout.fillWidth: true; label: Theme.t("源点 / 三角形", "Source points / triangles", "來源點 / 三角形"); value: (root.session.info.points ?? "—")+" / "+(root.session.info.triangles ?? "—") }
                    Readout { Layout.fillWidth: true; label: Theme.t("当前网格", "This mesh", "目前網格"); value: (root.session.info.meshPoints ?? "—")+" / "+(root.session.info.meshTriangles ?? "—") }
                    Readout { Layout.fillWidth: true; label: Theme.t("当前可见", "Currently visible", "目前可見"); value: (root.session.info.visiblePoints ?? "—")+" / "+(root.session.info.visibleTriangles ?? "—") }
                    Label { Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap; text: Explain.noteText(root.session.info.coordinates || ""); font.pixelSize: Theme.noteText; textFormat: Text.PlainText }
                }
                SectionHeader { id: issues; objectName: "modelSourceResources"; Layout.fillWidth: true; text: Theme.t("资源与说明", "Resources and notes", "資源與說明")+" · "+(root.session.resources.length+root.session.warnings.length) }
                ColumnLayout {
                    Layout.fillWidth: true; visible: issues.checked
                    Repeater {
                        model: root.session.warnings
                        delegate: ExplainedText { required property string modelData; objectName: "modelNote"; Layout.fillWidth: true; kind: "note"; raw: modelData; tone: Theme.muted }
                    }
                    Repeater {
                        model: root.session.resources
                        delegate: Readout { required property var modelData; Layout.fillWidth: true; label: modelData.reference || modelData.resolved || ""; value: modelData.loaded ? Theme.t("已读取", "Read", "已讀取") : modelData.detail || Theme.t("未读取", "Not read", "未讀取") }
                    }
                }
            }
        }
        ModelSkeletonPane {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: root.tab===1
            session: root.session
        }
        ScrollView {
            id: materialScroll
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: root.tab===2
            contentWidth: availableWidth
            ColumnLayout {
                width: materialScroll.availableWidth; spacing: 10
                Label { text: Theme.t("基础颜色 · 会话关联", "Base colour · session association", "基礎顏色 · 工作階段關聯"); color: Theme.text; font.bold: true }
                Choice { Layout.fillWidth: true; model: root.session.materials; currentIndex: root.session.materialIndex; onActivated: root.session.selectMaterial(currentIndex); Accessible.name: Theme.t("材质槽", "Material slot", "材質槽") }
                Readout { Layout.fillWidth: true; label: Theme.t("源引用", "Source reference", "來源參照"); value: root.session.textureReference || "—" }
                Readout { Layout.fillWidth: true; label: Theme.t("已验证贴图", "Validated texture", "已驗證貼圖"); value: root.session.texturePath || "—" }
                Choice { Layout.fillWidth: true; model: root.session.uvSets; currentIndex: root.session.uvIndex; enabled: count>0; onActivated: root.session.selectUV(currentIndex); Accessible.name: Theme.t("会话关联使用的UV集", "UV set for session association", "工作階段關聯使用的UV集") }
                Tool { Layout.fillWidth: true; text: root.session.textureBusy ? Theme.t("读取中…", "Reading…", "讀取中…") : Theme.t("定位贴图…", "Locate texture…", "定位貼圖…"); enabled: root.session.available&&!root.session.textureBusy; onClicked: textureDialog.open() }
                Tool { Layout.fillWidth: true; text: Theme.t("清除会话关联", "Clear session association", "清除工作階段關聯"); enabled: root.session.canClearTexture; onClicked: root.session.removeTexture() }
                ExplainedText { objectName: "modelTextureError"; Layout.fillWidth: true; kind: "error"; raw: root.session.textureError }
                Label { Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap; font.pixelSize: Theme.noteText; text: Theme.t("定位后仍须真实解码；只改变本次显示，不保存或覆盖源模型、材质或贴图。", "Located images must decode successfully. Associations only affect this session and never overwrite source models, materials or textures.", "定位後仍須實際解碼；僅改變本次顯示，不儲存或覆寫來源模型、材質或貼圖。") }
            }
        }
    }
    FileDialog { id: textureDialog; title: Theme.t("选择基础颜色贴图", "Choose a base colour texture", "選取基礎顏色貼圖"); onAccepted: root.session.associateTexture(selectedFile.toString()) }
}
