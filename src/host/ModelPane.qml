pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Rectangle {
    id: root
    objectName: "modelPane"
    required property var controller
    property bool focused: false
    readonly property var session: controller.model
    readonly property var uvBackground: session.uvBackground
    readonly property bool narrow: width < 700 || (root.Window.window && root.Window.window.width <= 1300)
    readonly property ModelViewport uvViewport: uvLoader.status === Loader.Ready ? uvLoader.item as ModelViewport : null
    property bool showUV: true
    property bool showCompare: false
    property bool showGrid: true
    property bool showAxes: true
    property bool wireframe: false
    property var uvCamera: ({})
    Connections {
        target: root.session
        function onDocumentChanged() {
            if (root.session.path !== "") return
            // The pane survives model opens; preserve the fresh-document defaults.
            root.showUV = true
            root.showCompare = false
            root.showGrid = true
            root.showAxes = true
            root.wireframe = false
            root.uvCamera = ({})
            externalDialog.close()
            compareDialog.close()
        }
    }
    color: Theme.base
    function addExternalAnimation(): void {
        if (root.session.canAttachExternalAnimation && !root.session.busy && !root.session.externalBusy) externalDialog.open()
    }
    function fit(selected: bool): void {
        if (root.narrow && root.session.mode===1 && root.showUV && root.uvViewport) root.uvViewport.fit(selected)
        else modelViewport.fit(selected)
    }
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.margins: 8; spacing: 6
            Repeater {
                model: root.narrow ? [Theme.t("模型", "Model", "模型"), "UV", Theme.t("权重", "Weights", "權重"), "LOD"] : [Theme.t("模型", "Model", "模型"), "UV", Theme.t("源权重", "Source weights", "來源權重"), Theme.t("LOD/碰撞", "LOD/Collision", "LOD/碰撞")]
                delegate: PageTab {
                    Layout.minimumWidth: 40
                    required property int index
                    required property string modelData
                    objectName: "modelModeTab" + index
                    text: modelData; checked: root.session.mode===index
                    onClicked: root.session.setMode(index)
                }
            }
            Tool {
                objectName: "animExternalAttach"
                Layout.minimumWidth: 32
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth
                visible: root.session.canAttachExternalAnimation || root.session.externalInfo.driven === true
                text: Theme.t("添加外部动画…", "Add external animation…", "新增外部動畫…")
                hint: root.session.externalInfo.format === "psa"
                      ? Theme.t("选择匹配当前 PSK 的 PSA 动画，自动读取同名轨道配置", "Choose PSA animations matching this PSK; matching track configs load automatically", "選擇符合目前 PSK 的 PSA 動畫，自動讀取同名軌道設定")
                      : Theme.t("选择一个或多个带骨架动画的 FBX，驱动当前模型", "Choose one or more skeletal-animation FBX files to drive this model", "選擇一個或多個帶骨架動畫的 FBX，驅動目前模型")
                enabled: !root.session.busy && !root.session.externalBusy
                onClicked: root.addExternalAnimation()
            }
            Item { Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 0 }
            Tool { objectName: "modelWireframe"; Layout.preferredWidth: 32; iconName: "wireframe"; hint: Theme.t("线框模式", "Wireframe", "線框模式"); checkable: true; checked: root.wireframe; enabled: root.session.available; onToggled: root.wireframe=checked }
            Tool { objectName: "modelGroundGrid"; Layout.preferredWidth: 32; visible: root.session.mode!==1 || !root.narrow || !root.showUV; iconName: "ground-grid"; hint: Theme.t("地面网格", "Ground grid", "地面網格"); checkable: true; checked: root.showGrid; enabled: root.session.available; onToggled: root.showGrid=checked }
            Tool { objectName: "modelOriginAxes"; Layout.preferredWidth: 32; visible: root.session.mode!==1 || !root.narrow || !root.showUV; iconName: "origin-axes"; hint: Theme.t("原点坐标轴 · X 红 / Y 绿 / Z 蓝", "Origin axes · X red / Y green / Z blue", "原點座標軸 · X 紅 / Y 綠 / Z 藍"); checkable: true; checked: root.showAxes; enabled: root.session.available; onToggled: root.showAxes=checked }
            Tool { objectName: "modelFocus"; Layout.preferredWidth: 32; iconName: "focus"; hint: Theme.t("聚焦所选对象（F）", "Focus selection (F)", "聚焦所選物件（F）"); enabled: root.session.available; onClicked: root.fit(true) }
            Tool { objectName: "modelFrameAll"; Layout.preferredWidth: 32; iconName: "frame-all"; hint: Theme.t("显示全部（Home）", "Frame all (Home)", "顯示全部（Home）"); enabled: root.session.available; onClicked: root.fit(false) }
            Tool { objectName: "modelCompareSkeleton"; Layout.minimumWidth: 32; Layout.fillWidth: true; Layout.maximumWidth: implicitWidth; visible: root.session.hasComparableSkeleton && !root.session.compareBusy; text: Theme.t("骨架对比…", "Compare skeletons…", "骨架對比…"); hint: Theme.t("与另一 FBX / glTF 比较骨架层级", "Compare the skeleton hierarchy with another FBX / glTF", "與另一 FBX / glTF 比較骨架層級"); enabled: root.session.available && !root.session.busy; onClicked: compareDialog.open(); Accessible.name: hint }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
            visible: root.session.mode===1
            Label { visible: !root.narrow; text: Theme.t("检查 UV 集", "Inspected UV set", "檢查 UV 集"); color: Theme.muted; font.pixelSize: Theme.noteText }
            Choice { objectName: "modelUVChoice"; Layout.fillWidth: true; model: root.session.uvSets; currentIndex: root.session.uvIndex; enabled: count>0; onActivated: root.session.selectUV(currentIndex); Accessible.name: Theme.t("UV 集", "UV set", "UV 集") }
            PageTab { visible: root.narrow; text: Theme.t("三维", "3D", "三維"); checked: !root.showUV; onClicked: root.showUV=false }
            PageTab { visible: root.narrow; text: "UV"; checked: root.showUV; onClicked: root.showUV=true }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10; spacing: 8
            visible: root.session.mode===1
            Label { visible: !root.narrow; text: Theme.t("参考材质", "Reference material", "參考材質"); color: Theme.muted; font.pixelSize: Theme.noteText }
            Choice {
                objectName: "modelUVMaterialChoice"
                Layout.fillWidth: true; Layout.minimumWidth: 100
                model: root.session.materials; currentIndex: root.session.materialIndex; enabled: count>0
                onActivated: root.session.selectMaterial(currentIndex)
                Accessible.name: Theme.t("UV 参考材质", "UV reference material", "UV 參考材質")
            }
            OptionCheck {
                objectName: "modelUVMaterialOnly"
                text: Theme.t("仅此材质", "Only this material", "僅此材質")
                checked: root.session.materialOnly; enabled: root.session.materials.length>0
                onToggled: root.session.setMaterialOnly(checked)
            }
        }
        Label {
            objectName: "modelUVBackgroundLegend"
            Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10; Layout.preferredHeight: 24
            visible: root.session.mode===1
            color: Theme.muted; font.pixelSize: Theme.noteText; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
            text: root.session.uvSets.length===0
                  ? Theme.t("无 UV · 不生成替代坐标", "No UV · no substitute coordinates", "無 UV · 不產生替代座標")
                  : root.uvBackground.shown
                    ? Theme.t("0–1 参考格 · RGB 底图：%1 / %2", "0–1 reference grid · RGB background: %1 / %2", "0–1 參考格 · RGB 底圖：%1 / %2").arg(root.uvBackground.material).arg(root.uvBackground.uv)
                    : root.uvBackground.decoded && !root.uvBackground.matching
                      ? Theme.t("0–1 参考格 · 材质使用 %1，与当前 UV 不同，暂不显示底图", "0–1 reference grid · material uses %1; background hidden for this UV set", "0–1 參考格 · 材質使用 %1，與目前 UV 不同，暫不顯示底圖").arg(root.uvBackground.uv)
                      : Theme.t("0–1 参考格 · 当前材质没有可用底图", "0–1 reference grid · no usable image for this material", "0–1 參考格 · 目前材質沒有可用底圖")
            ToolTip.visible: legendHover.hovered
            ToolTip.text: text
            HoverHandler { id: legendHover }
        }
        Item {
            id: stage
            Layout.fillWidth: true; Layout.fillHeight: true
            RowLayout {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: animationBarHost.visible ? animationBarHost.top : parent.bottom
                spacing: 1
                ModelViewport {
                    id: modelViewport
                    Layout.fillWidth: true; Layout.fillHeight: true
                    session: root.session
                    showGrid: root.showGrid; showAxes: root.showAxes
                    wireframe: root.wireframe
                    visible: root.session.mode!==1 || !root.narrow || !root.showUV
                }
                Rectangle { Layout.fillHeight: true; Layout.preferredWidth: 1; visible: root.session.mode===1&&!root.narrow; color: Theme.line }
                Loader {
                    id: uvLoader
                    Layout.fillWidth: true; Layout.fillHeight: true
                    active: root.session.mode===1
                    visible: active && (!root.narrow || root.showUV)
                    sourceComponent: Component {
                        ModelViewport {
                            uv: true; session: root.session; initialCamera: root.uvCamera
                            onCameraSaved: state => root.uvCamera=state
                            Label { anchors.centerIn: parent; visible: !root.session.busy&&!root.session.preparing&&root.session.uvSets.length===0; text: Theme.t("当前网格没有 UV", "This mesh has no UV set", "目前網格沒有 UV"); color: Theme.muted }
                        }
                    }
                }
            }
            SkeletonComparePane {
                id: skeletonCompareHost
                objectName: "skeletonCompareHost"
                visible: {
                    if (!root.showCompare)
                        return false
                    const result = root.session.compareResult
                    if (result && result.structureVerdict !== undefined)
                        return true
                    return root.session.compareError.length > 0
                }
                enabled: visible
                session: root.session
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 16
                height: Math.min(parent.height - 32, 420)
                onClosed: root.showCompare = false
            }
            AnimationBar {
                id: animationBarHost
                objectName: "modelAnimationBarHost"
                visible: root.session.animated || root.session.canAttachExternalAnimation
                enabled: visible
                session: root.session
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 4
            }
            ColumnLayout {
                anchors.centerIn: parent; width: Math.min(stage.width-40,480)
                visible: root.session.busy || root.session.preparing || !!root.session.error || !!root.session.frameError
                BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: root.session.busy || root.session.preparing; visible: running }
                ExplainedText { objectName: "modelErrorExplained"; Layout.fillWidth: true; kind: "error"; raw: root.session.error || root.session.frameError }
                Label { visible: !root.session.error && !root.session.frameError; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; color: Theme.text; text: (root.session.preparing ? Theme.t("正在准备视图 · 旧画面仅供参考，精确检查暂缓", "Preparing view · previous frame is reference only; exact inspection paused", "正在準備視圖 · 舊畫面僅供參考，精確檢查暫緩") : Theme.t("正在读取模型…", "Reading model…", "正在讀取模型…")) }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Tool { visible: (!!root.session.error || !!root.session.frameError) && !root.session.error.startsWith("ANIMATION_ONLY_FBX:"); text: Theme.t("重试", "Retry", "重試"); onClicked: root.session.retry() }
                    Tool { visible: root.session.busy || root.session.preparing || !!root.session.error || !!root.session.frameError; text: Theme.t("取消", "Cancel", "取消"); onClicked: root.session.cancel() }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: root.session.available && !root.session.preparing && root.session.mode===2 && root.session.bindingIndex<0
                text: root.session.bindings.length ? Theme.t("请在检查器明确选择蒙皮绑定", "Choose a skin binding in the inspector", "請在檢查器明確選取蒙皮綁定") : Theme.t("当前网格没有蒙皮绑定", "This mesh has no skin binding", "目前網格沒有蒙皮綁定")
                color: Theme.muted
            }
        }
        Label {
            // Focus mode hides mouse-control hints to free preview space.
            visible: !root.focused
            Layout.fillWidth: true; Layout.margins: 8
            color: Theme.muted; font.pixelSize: Theme.noteText; elide: Text.ElideRight
            text: Theme.t("左键拖动旋转 · 中/右拖平移 · 滚轮缩放 · F 聚焦 · 单击检查源点", "Left drag orbit · Middle/right drag pan · Wheel zoom · F focus · Click to inspect", "左鍵拖曳旋轉 · 中/右拖平移 · 滾輪縮放 · F 聚焦 · 按一下檢查來源點")
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.line }
        ModelPointPanel { Layout.fillWidth: true; session: root.session }
    }
    FileDialog {
        id: externalDialog
        objectName: "externalAnimationDialog"
        readonly property bool actorTarget: root.session.externalInfo.format === "psa"
        title: actorTarget
               ? Theme.t("选择匹配当前 PSK 的 PSA 动画（可多选）", "Choose PSA animations matching this PSK (multiple allowed)", "選擇符合目前 PSK 的 PSA 動畫（可多選）")
               : Theme.t("选择驱动当前模型的动画 FBX（可多选）", "Choose animation FBX to drive this model (multiple allowed)", "選擇驅動目前模型的動畫 FBX（可多選）")
        nameFilters: actorTarget ? ["ActorX PSA (*.psa)"] : ["Autodesk FBX (*.fbx)"]
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            for (var i = 0; i < selectedFiles.length; ++i)
                root.session.attachExternalAnimation(selectedFiles[i].toString())
        }
    }
    FileDialog {
        id: compareDialog
        title: Theme.t("选择要对比的 FBX / glTF 骨架", "Choose the FBX skeleton to compare", "選擇要對比的 FBX / glTF 骨架")
        nameFilters: ["Skeleton assets (*.fbx *.gltf *.glb)", "Autodesk FBX (*.fbx)", "glTF (*.gltf *.glb)"]
        onAccepted: { root.showCompare = true; root.session.compareSkeletonWith(selectedFile.toString()) }
    }
}
