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
    readonly property var camera: session.cameraInfo
    function cameraValue(value, unit): string {
        return value === undefined || value === null || !isFinite(Number(value))
               ? Theme.t("源文件未提供 / 无法推导", "Not provided / cannot derive", "來源未提供 / 無法推導")
               : Number(value).toFixed(3) + unit
    }
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
                    enabled: !root.session.info.cameraOnly || index===0
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
                SectionHeader { id: camerasSection; objectName: "modelCameras"; Layout.fillWidth: true; visible: root.session.cameras.length>0; checked: true; text: Theme.t("资产相机 · 轨迹与视锥", "Asset cameras · path and frustum", "資產相機 · 軌跡與視錐") }
                ColumnLayout {
                    Layout.fillWidth: true; visible: camerasSection.visible && camerasSection.checked
                    Choice { objectName: "modelCameraChoice"; Layout.fillWidth: true; model: root.session.cameras; currentIndex: root.session.cameraIndex; onActivated: root.session.selectCamera(currentIndex); Accessible.name: Theme.t("选择资产相机", "Select asset camera", "選取資產相機") }
                    Readout { Layout.fillWidth: true; label: Theme.t("源路径 · 当前时间", "Source path · current time", "來源路徑 · 目前時間"); value: (root.camera.path || "—") + " · " + Number(root.camera.time || 0).toFixed(3) + " s" }
                    Readout { Layout.fillWidth: true; label: Theme.t("投影 · 局部轴向", "Projection · local axes", "投影 · 局部軸向"); value: (root.camera.projection || "unknown") + " · " + (root.camera.axes || "unknown") }
                    Readout { Layout.fillWidth: true; label: Theme.t("源世界位置", "Source world position", "來源世界位置"); value: root.camera.position || "—" }
                    Readout { Layout.fillWidth: true; label: Theme.t("水平 / 垂直 FOV", "Horizontal / vertical FOV", "水平 / 垂直 FOV"); value: root.cameraValue(root.camera.fovX,"°") + " / " + root.cameraValue(root.camera.fovY,"°") }
                    Readout { Layout.fillWidth: true; label: Theme.t("焦距", "Focal length", "焦距"); value: root.cameraValue(root.camera.focalMm," mm") }
                    Readout { Layout.fillWidth: true; label: Theme.t("水平 / 垂直 filmback", "Horizontal / vertical filmback", "水平 / 垂直 filmback"); value: root.cameraValue(root.camera.apertureXmm," mm") + " / " + root.cameraValue(root.camera.apertureYmm," mm") }
                    Readout { Layout.fillWidth: true; label: Theme.t("近 / 远裁剪", "Near / far clipping", "近 / 遠裁剪"); value: root.cameraValue(root.camera.near,"") + " / " + root.cameraValue(root.camera.far,"") }
                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; font.pixelSize: Theme.noteText; text: (root.camera.fovSource || "") + " · " + (root.camera.unit || ""); textFormat: Text.PlainText }
                }
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
                SectionHeader { id: cacheSection; objectName: "modelGeometryCache"; Layout.fillWidth: true; visible: root.session.geometryCache; checked: true; text: Theme.t("几何缓存", "Geometry cache", "幾何快取") }
                ColumnLayout {
                    id: cacheDetails
                    objectName: "modelGeometryCacheDetails"
                    readonly property var cache: root.session.cacheInfo
                    readonly property var meshRows: root.session.cacheMeshes
                    Layout.fillWidth: true; visible: root.session.geometryCache && cacheSection.checked
                    Readout { objectName: "cacheTimelineReadout"; Layout.fillWidth: true; label: Theme.t("时间轴采样 / 当前", "Timeline samples / current", "時間軸取樣 / 目前"); value: cacheDetails.cache.samples !== undefined ? cacheDetails.cache.samples + " / " + cacheDetails.cache.sample + " · " + Number(cacheDetails.cache.time).toFixed(3) + " s" : "—" }
                    Readout { objectName: "cacheSampleStatistics"; Layout.fillWidth: true; label: Theme.t("点 / 面 / 角 / 三角形", "Points / faces / corners / triangles", "點 / 面 / 角 / 三角形"); value: cacheDetails.cache.points !== undefined ? [cacheDetails.cache.points, cacheDetails.cache.faces, cacheDetails.cache.corners, cacheDetails.cache.triangles].join(" / ") : "—" }
                    Readout { objectName: "cacheWriter"; Layout.fillWidth: true; label: Theme.t("写出程序 · 单位", "Writer · units", "寫出程式 · 單位"); value: (cacheDetails.cache.application || Theme.t("未记录", "not recorded", "未記錄")) + " · " + (cacheDetails.cache.unitKnown ? (cacheDetails.cache.metresPerUnit === 0.01 ? Theme.t("厘米（按写出程序）", "centimetres (from writer)", "公分（依寫出程式）") : Theme.t("米（按写出程序）", "metres (from writer)", "公尺（依寫出程式）")) : Theme.t("未知，按米约定", "unknown; metre convention", "未知，依公尺約定")) }
                    Readout { objectName: "cacheResidency"; Layout.fillWidth: true; label: Theme.t("已缓存采样 · 读取次数", "Cached samples · reads", "已快取取樣 · 讀取次數"); value: cacheDetails.cache.cachedSamples !== undefined ? Theme.t("%1 个（%2 MiB，上限 %3 个 / %4 MiB）· 读取 %5 次", "%1 (%2 MiB, limit %3 / %4 MiB) · %5 reads", "%1 個（%2 MiB，上限 %3 個 / %4 MiB）· 讀取 %5 次").arg(cacheDetails.cache.cachedSamples).arg((cacheDetails.cache.cachedBytes / 1048576).toFixed(1)).arg(cacheDetails.cache.cacheLimitSamples).arg((cacheDetails.cache.cacheLimitBytes / 1048576).toFixed(0)).arg(cacheDetails.cache.reads) : "—" }
                    Repeater {
                        // Keep row objects while sample metadata changes. A
                        // QVariantList model recreates every Readout per frame.
                        model: cacheDetails.meshRows.length
                        delegate: Readout {
                            required property int index
                            readonly property var modelData: cacheDetails.meshRows[index] || ({})
                            objectName: "cacheMeshReadout"
                            Layout.fillWidth: true
                            label: modelData.path
                            value: (modelData.topology === "varying" ? Theme.t("变化拓扑", "Varying topology", "變化拓撲") : modelData.topology === "static" ? Theme.t("静态", "Static", "靜態") : Theme.t("固定拓扑", "Fixed topology", "固定拓撲"))
                                   + " · " + Theme.t("采样 %1（当前 %2）", "%1 samples (current %2)", "取樣 %1（目前 %2）").arg(modelData.samples).arg(modelData.sample)
                                   + " · UV " + (modelData.uv ? Theme.t("有", "yes", "有") : Theme.t("无", "no", "無"))
                                   + " · " + Theme.t("法线", "normals", "法線") + " " + (modelData.normals ? Theme.t("有", "yes", "有") : Theme.t("无", "no", "無"))
                                   + " · " + Theme.t("点 %1 · 角 %2 · 三角形 %3", "%1 points · %2 corners · %3 triangles", "點 %1 · 角 %2 · 三角形 %3").arg(modelData.points).arg(modelData.corners).arg(modelData.triangles)
                        }
                    }
                    Label { visible: (cacheDetails.cache.skipped || 0) > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.warning; font.pixelSize: Theme.noteText; text: Theme.t("%1 个非多边形对象（曲线、点、细分曲面等）未预览，详见资源与说明。", "%1 non-polygon objects (curves, points, subdivision surfaces…) are not previewed; see Resources and notes.", "%1 個非多邊形物件（曲線、點、細分曲面等）未預覽，詳見資源與說明。").arg(cacheDetails.cache.skipped || 0) }
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
            visible: root.tab===1 && !root.session.geometryCache
            session: root.session
        }
        Label {
            objectName: "modelSkeletonUnavailable"
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: root.tab===1 && root.session.geometryCache
            verticalAlignment: Text.AlignTop; wrapMode: Text.Wrap; color: Theme.muted
            text: Theme.t("几何缓存（Alembic）只包含逐采样的网格与变换，没有骨架、蒙皮权重或根运动；这些检查在 ABC 模式不可用，也不会被推断。", "Geometry caches (Alembic) contain per-sample meshes and transforms only, with no skeleton, skin weights or root motion; these inspections are unavailable for ABC and are never inferred.", "幾何快取（Alembic）只包含逐取樣的網格與變換，沒有骨架、蒙皮權重或根運動；這些檢查在 ABC 模式不可用，也不會被推斷。")
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
