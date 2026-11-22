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
    property Item headerSlot
    readonly property int toolsWidth: modeTools.implicitWidth
    readonly property bool inlineHeader: !!headerSlot && width >= toolsWidth + 230
    signal inspectSection(string section)
    property string section: "model"
    function selectSection(key, mode) {
        section = key
        session.setMode(mode)
        session.setSkeletonOverlay(key === "skeleton" || key === "binding")
        if (key === "animation") {
            animationBarHost.collapsed = false
            animationBarHost.optionsVisible = false
        }
        inspectSection(key)
    }
    readonly property var session: controller.model
    readonly property var uvBackground: session.uvBackground
    readonly property bool narrow: width < 700 || (root.Window.window && root.Window.window.width <= 1300)
    readonly property ModelViewport uvViewport: uvLoader.status === Loader.Ready ? uvLoader.item as ModelViewport : null
    property bool showUV: true
    property bool showCompare: false
    property bool showGrid: false
    property bool showAxes: false
    property int previewMode: 0
    readonly property var shadingOrder: [0, 1, 4, 2, 3]
    readonly property var shadingLabels: [Theme.t("材质","Material","材質"), Theme.t("棋盘格","Checker","棋盤格"),
        Theme.t("顶点色","Vertex color","頂點色"), Theme.t("线框","Wireframe","線框"), Theme.t("素模","Clay","素模")]
    readonly property string triangleText: Theme.t("%1 三角形","%1 triangles","%1 三角形")
        .arg(Number(session.info.visibleTriangles ?? session.info.triangles ?? 0).toLocaleString(Qt.locale(), "f", 0))
    readonly property bool wireframe: previewMode === 3
    property var uvCamera: ({})
    Connections {
        target: root.session
        function onDocumentChanged() {
            if (root.session.path !== "") return
            // The pane survives model opens; preserve the fresh-document defaults.
            root.showUV = true
            root.showCompare = false
            root.showGrid = false
            root.showAxes = false
            root.section = "model"
            root.previewMode = 0
            root.uvCamera = ({})
            externalDialog.close()
            compareDialog.close()
        }
    }
    color: Theme.light ? "#edf1f4" : "#232d37"
    function addExternalAnimation(): void {
        if (root.session.canAttachExternalAnimation && !root.session.busy && !root.session.externalBusy) externalDialog.present()
    }
    function fit(selected: bool): void {
        if (root.narrow && root.session.mode===1 && root.showUV && root.uvViewport) root.uvViewport.fit(selected)
        else modelViewport.fit(selected)
    }
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        Item {
            id: modesFallback
            Layout.fillWidth: true
            implicitHeight: root.inlineHeader ? 0 : 38
            visible: !root.inlineHeader
            Flickable {
                id: modeStrip
                parent: root.inlineHeader ? root.headerSlot : modesFallback
                anchors.fill: parent
                contentWidth: modeTools.implicitWidth
                clip: true
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                Row {
                    id: modeTools
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Rectangle {
                        radius: 9; color: Theme.light ? "#405e7889" : "#504e6476"
                        width: group.implicitWidth + 6; height: 30
                        Row {
                            id: group
                            anchors.centerIn: parent
                            Repeater {
                                model: [
                                    ["model", 0, Theme.t("模型","Model","模型")],
                                    ["uv", 1, "UV"],
                                    ["skeleton", 0, Theme.t("骨骼","Skeleton","骨骼")],
                                    ["weights", 2, Theme.t("权重","Weights","權重")],
                                    ["binding", 0, Theme.t("绑定","Binding","綁定")],
                                    ["lod", 3, Theme.t("LOD / 碰撞","LOD / Collision","LOD / 碰撞")],
                                    ["animation", 0, Theme.t("动画","Animation","動畫")]
                                ]
                                delegate: PageTab {
                                    required property var modelData
                                    readonly property string sectionKey: modelData[0]
                                    objectName: ["model","uv","weights","lod"].includes(sectionKey) ? "modelModeTab"+modelData[1] : "modelSection_"+sectionKey
                                    text: modelData[2]; height: 26; padding: 4; implicitWidth: implicitContentWidth + 14
                                    checked: modelData[1] === root.session.mode && (modelData[1] !== 0 || root.section === sectionKey || (sectionKey==="model" && ["uv","weights","lod"].includes(root.section)))
                                    enabled: sectionKey === "model" || (!root.session.info.cameraOnly &&
                                        (sectionKey==="animation" ? (root.session.hasPlayableAnimation || root.session.canAttachExternalAnimation || animationBarHost.hasDiagnostics) :
                                        ["skeleton","binding","weights"].includes(sectionKey) ? !root.session.geometryCache && (sectionKey==="weights" || root.session.hasComparableSkeleton || root.session.bindings.length>0) : true))
                                    onClicked: root.selectSection(sectionKey, modelData[1])
                                }
                            }
                        }
                    }
                    Tool {
                        objectName: "animExternalAttach"; height: 30; quiet: true; iconName: "external"
                        visible: root.session.canAttachExternalAnimation || root.session.externalInfo.driven === true
                        hint: Theme.t("添加外部动画…","Add external animation…","新增外部動畫…")
                        enabled: !root.session.busy && !root.session.externalBusy
                        onClicked: root.addExternalAnimation()
                    }
                }
            }
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
                anchors.bottom: displayTools.top
                spacing: 1
                ModelViewport {
                    id: modelViewport
                    Layout.fillWidth: true; Layout.fillHeight: true
                    session: root.session
                    showGrid: root.showGrid; showAxes: root.showAxes
                    wireframe: root.wireframe
                    shadingMode: root.previewMode === 4 ? 3 : root.wireframe ? 0 : root.previewMode
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
            Label {
                anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 8
                visible: text.length > 0 && !root.session.busy
                color: Theme.text
                padding: 4
                background: Rectangle { color: Theme.panel; radius: Theme.radius }
                text: [
                    (root.session.directionMask & 1) && !root.session.normalDirectionsAvailable ? Theme.t("无可用法线", "No normal data", "無可用法線") : "",
                    !(root.session.directionMask & 2) ? "" :
                    root.session.tangentSource === "source" ? Theme.t("切线 · 源数据", "Tangents · source", "切線 · 來源資料") :
                    root.session.tangentSource === "uv" ? Theme.t("切线 · 由 UV 计算", "Tangents · from UVs", "切線 · 由 UV 計算") :
                    root.session.tangentSource === "mixed" ? Theme.t("切线 · 源数据及 UV 推算", "Tangents · source and UVs", "切線 · 來源資料及 UV 推算") :
                    Theme.t("无可用切线或有效 UV", "No tangents or valid UVs", "無可用切線或有效 UV")
                ].filter(Boolean).join(" · ")
            }
            Label {
                objectName: "modelSectionLegend"
                anchors.left: parent.left; anchors.bottom: displayTools.top; anchors.margins: 10
                width: Math.min(implicitWidth, parent.width - 20)
                visible: root.session.inspectable && (root.section === "skeleton" || root.session.mode === 3)
                padding: 6; leftPadding: 10; rightPadding: 10
                color: Theme.text; font.pixelSize: Theme.noteText
                textFormat: Text.PlainText; elide: Text.ElideRight
                background: Rectangle { color: Theme.panel; radius: height / 2; border.color: Theme.line }
                text: {
                    if (!visible) return ""
                    if (root.session.mode === 3) {
                        const collision = [Theme.t("隐藏碰撞","Hide collision","隱藏碰撞"),
                                           Theme.t("叠加碰撞","Overlay collision","疊加碰撞"),
                                           Theme.t("仅碰撞","Collision only","僅碰撞")]
                        return collision[root.session.collisionMode] + " · " + root.triangleText
                    }
                    return root.session.skeletonSelection.name || Theme.t("选择骨骼以查看层级","Select a bone to inspect","選擇骨骼以查看階層")
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
            RowLayout {
                id: displayTools
                anchors.left: parent.left; anchors.right: parent.right
                anchors.bottom: animationBarHost.visible ? animationBarHost.top : cacheTimelineHost.visible ? cacheTimelineHost.top : parent.bottom
                anchors.leftMargin: 8; anchors.rightMargin: 8; anchors.bottomMargin: 4; spacing: 4

            Rectangle {
                visible: displayTools.width >= 660
                Layout.preferredWidth: shadingOptions.implicitWidth + 6; implicitHeight: 30; radius: 9
                color: Theme.light ? "#365b788c" : "#494e6476"
                Row {
                    id: shadingOptions
                    anchors.centerIn: parent
                    Repeater {
                        model: root.shadingOrder
                        delegate: PageTab {
                            required property int modelData
                            objectName: "modelShadingOption"+modelData
                            text: root.shadingLabels[modelData]; height: 26; padding: 3; implicitWidth: implicitContentWidth+12
                            checked: root.previewMode === modelData
                            enabled: root.session.available && !root.session.info.cameraOnly
                            onClicked: root.previewMode=modelData
                        }
                    }
                }
            }
            Tool {
                id: modeButton
                visible: displayTools.width < 660
                objectName: "modelPreviewMode"
                Layout.preferredWidth: Theme.controlHeight
                readonly property var icons: ["display-shaded", "display-checker", "display-vertices", "display-wire", "display-shaded"]
                readonly property var labels: root.shadingLabels
                readonly property var notes: ["", Theme.t("使用 UV 棋盘格；无 UV 时使用空间投影", "UV checker; spatial projection when UVs are absent", "使用 UV 棋盤格；無 UV 時使用空間投影"), Theme.t("显示第一组顶点 RGB；无顶点色时显示中性灰", "First vertex RGB set; neutral gray when absent", "顯示第一組頂點 RGB；無頂點色時顯示中性灰"), "", Theme.t("使用中性材质检查表面形体","Inspect form with a neutral material","使用中性材質檢查表面形體")]
                iconName: icons[root.previewMode]
                hint: Theme.t("预览显示模式", "Preview display mode", "預覽顯示模式") + " · " + labels[root.previewMode]
                Accessible.name: hint
                enabled: root.session.available && !root.session.info.cameraOnly
                onClicked: modes.open()
                Keys.onDownPressed: modes.open()
                Keys.onReturnPressed: modes.open()
                Keys.onEnterPressed: modes.open()
                AppPopupMenu {
                    id: modes
                    objectName: "modelDisplayMenu"
                    y: -height - 2
                    width: 206
                    onOpened: itemAt(root.shadingOrder.indexOf(root.previewMode)).forceActiveFocus(Qt.PopupFocusReason)
                    onClosed: modeButton.forceActiveFocus(Qt.PopupFocusReason)
                    component DisplayOption: AppMenuItem {
                        id: option
                        required property int mode
                        objectName: "modelDisplayOption" + mode
                        text: modeButton.labels[mode]
                        leftPadding: 54
                        checkable: true
                        autoExclusive: true
                        checked: root.previewMode === mode
                        Accessible.description: modeButton.notes[mode]
                        ToolTip.visible: hovered && modeButton.notes[mode].length > 0
                        ToolTip.text: modeButton.notes[mode]
                        ToolTip.delay: 350
                        AppIcon { x: 28; anchors.verticalCenter: parent.verticalCenter; name: modeButton.icons[option.mode]; size: 18 }
                        onTriggered: root.previewMode = mode
                    }
                    DisplayOption { mode: 0 }
                    DisplayOption { mode: 1 }
                    DisplayOption { mode: 4 }
                    DisplayOption { mode: 2 }
                    DisplayOption { mode: 3 }
                }
            }
            Tool { objectName: "modelGroundGrid"; quiet: true; Layout.preferredWidth: 32; visible: root.session.mode!==1 || !root.narrow || !root.showUV; iconName: "ground-grid"; hint: Theme.t("地面网格", "Ground grid", "地面網格"); checkable: true; checked: root.showGrid; enabled: root.session.available; onToggled: root.showGrid=checked }
            Tool {
                objectName: "modelNormals"; text: "N"; checked: !!(root.session.directionMask & 1); quiet: true
                enabled: root.session.available; visible: root.session.mode !== 1 || !root.showUV || !root.narrow
                hint: Theme.t("法线方向（蓝色）", "Normal directions (blue)", "法線方向（藍色）")
                onClicked: root.session.directionMask ^= 1
            }
            Tool {
                objectName: "modelTangents"; text: "T"; checked: !!(root.session.directionMask & 2); quiet: true
                enabled: root.session.available; visible: root.session.mode !== 1 || !root.showUV || !root.narrow
                hint: Theme.t("切线方向（红色）", "Tangent directions (red)", "切線方向（紅色）")
                onClicked: root.session.directionMask ^= 2
            }
            Tool { objectName: "modelOriginAxes"; quiet: true; Layout.preferredWidth: 32; visible: root.session.mode!==1 || !root.narrow || !root.showUV; iconName: "origin-axes"; hint: Theme.t("原点坐标轴 · X 红 / Y 绿 / Z 蓝", "Origin axes · X red / Y green / Z blue", "原點座標軸 · X 紅 / Y 綠 / Z 藍"); checkable: true; checked: root.showAxes; enabled: root.session.available; onToggled: root.showAxes=checked }
            Tool { objectName: "modelFocus"; quiet: true; Layout.preferredWidth: 32; iconName: "focus"; hint: Theme.t("聚焦所选对象（F）", "Focus selection (F)", "聚焦所選物件（F）"); enabled: root.session.available; onClicked: root.fit(true) }
            Tool { objectName: "modelFrameAll"; quiet: true; Layout.preferredWidth: 32; iconName: "frame-all"; hint: Theme.t("显示全部（Home）", "Frame all (Home)", "顯示全部（Home）"); enabled: root.session.available; onClicked: root.fit(false) }
            Tool { objectName: "modelCompareSkeleton"; Layout.minimumWidth: 32; Layout.fillWidth: true; Layout.maximumWidth: implicitWidth; visible: root.session.hasComparableSkeleton && !root.session.compareBusy; iconName: "weights"; quiet: true; hint: Theme.t("与另一 FBX / glTF 比较骨架层级", "Compare the skeleton hierarchy with another FBX / glTF", "與另一 FBX / glTF 比較骨架層級"); enabled: root.session.available && !root.session.busy; onClicked: compareDialog.present(); Accessible.name: hint }
                Item { Layout.fillWidth: true }
                Label { visible: !root.narrow; text: root.triangleText; color: Theme.muted }
            }
            AnimationBar {
                id: animationBarHost
                objectName: "modelAnimationBarHost"
                visible: root.session.hasPlayableAnimation || animationBarHost.hasDiagnostics
                enabled: visible
                session: root.session
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 4
            }
            CacheTimeline {
                id: cacheTimelineHost
                objectName: "modelCacheTimelineHost"
                visible: root.session.geometryCache
                enabled: visible
                session: root.session
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 4
            }
            ColumnLayout {
                anchors.centerIn: parent; width: Math.min(stage.width-40,480)
                // Geometry-cache sample changes report progress in the timeline
                // instead of covering the previous sample during playback.
                visible: root.session.busy || (root.session.preparing && !root.session.geometryCache) || !!root.session.error || !!root.session.frameError
                BusyIndicator {
                    id: modelBusyIndicator
                    Layout.alignment: Qt.AlignHCenter
                    readonly property bool loading: root.session.busy || root.session.preparing
                    property bool delayElapsed: false
                    onLoadingChanged: if (!loading) delayElapsed = false
                    // Keep status text and cancellation immediate; short reads
                    // need no spinner or render-thread animation between keys.
                    running: loading && delayElapsed
                    visible: modelBusyIndicator.running
                    Timer {
                        interval: 150
                        running: modelBusyIndicator.loading
                        onTriggered: modelBusyIndicator.delayElapsed = true
                    }
                    // Stop the Basic style's animation when loading ends,
                    // including its otherwise hidden opacity fade.
                    contentItem.visible: modelBusyIndicator.running
                    contentItem.opacity: 1
                }
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
    }
    FrostFileDialog {
        parentWindow: root.Window.window
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
    FrostFileDialog {
        parentWindow: root.Window.window
        id: compareDialog
        title: Theme.t("选择要对比的 FBX / glTF 骨架", "Choose the FBX skeleton to compare", "選擇要對比的 FBX / glTF 骨架")
        nameFilters: ["Skeleton assets (*.fbx *.gltf *.glb)", "Autodesk FBX (*.fbx)", "glTF (*.gltf *.glb)"]
        onAccepted: { root.showCompare = true; root.session.compareSkeletonWith(selectedFile.toString()) }
    }
}
