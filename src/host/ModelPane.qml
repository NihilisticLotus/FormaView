pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "modelPane"
    required property var controller
    readonly property var session: controller.model
    readonly property var uvBackground: session.uvBackground
    readonly property bool narrow: width < 700 || (root.Window.window && root.Window.window.width <= 1300)
    readonly property ModelViewport uvViewport: uvLoader.status === Loader.Ready ? uvLoader.item as ModelViewport : null
    property bool showUV: true
    property bool showGrid: true
    property bool showAxes: true
    property var uvCamera: ({})
    color: Theme.base
    function fit(selected: bool): void {
        if (root.narrow && root.session.mode===1 && root.showUV && root.uvViewport) root.uvViewport.fit(selected)
        else modelViewport.fit(selected)
    }
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 8; spacing: 6
            Repeater {
                model: root.narrow ? [Theme.t("模型", "Model", "模型"), "UV", Theme.t("权重", "Weights", "權重"), "LOD"] : [Theme.t("模型", "Model", "模型"), "UV", Theme.t("源权重", "Source weights", "來源權重"), Theme.t("LOD/碰撞", "LOD/Collision", "LOD/碰撞")]
                delegate: PageTab {
                    required property int index
                    required property string modelData
                    objectName: "modelModeTab" + index
                    text: modelData; checked: root.session.mode===index
                    onClicked: root.session.setMode(index)
                }
            }
            Item { Layout.fillWidth: true }
            Tool { objectName: "modelGroundGrid"; visible: root.session.mode!==1 || !root.narrow || !root.showUV; text: Theme.t("地面网格", "Ground grid", "地面網格"); checkable: true; checked: root.showGrid; enabled: root.session.available; onToggled: root.showGrid=checked }
            Tool { objectName: "modelOriginAxes"; visible: root.session.mode!==1 || !root.narrow || !root.showUV; text: Theme.t("原点坐标轴", "Origin axes", "原點座標軸"); hint: Theme.t("源坐标原点：X 红 / Y 绿 / Z 蓝", "Source origin: X red / Y green / Z blue", "來源座標原點：X 紅 / Y 綠 / Z 藍"); checkable: true; checked: root.showAxes; enabled: root.session.available; onToggled: root.showAxes=checked }
            Tool { visible: !root.narrow; text: Theme.t("聚焦 F", "Focus F", "聚焦 F"); enabled: root.session.available; onClicked: root.fit(true) }
            Tool { visible: !root.narrow; text: Theme.t("全部", "Frame all", "全部"); enabled: root.session.available; onClicked: root.fit(false) }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8
            visible: root.narrow
            Item { Layout.fillWidth: true }
            Tool { text: Theme.t("聚焦 F", "Focus F", "聚焦 F"); enabled: root.session.available; onClicked: root.fit(true) }
            Tool { text: Theme.t("全部", "Frame all", "全部"); enabled: root.session.available; onClicked: root.fit(false) }
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
                anchors.fill: parent; spacing: 1
                ModelViewport {
                    id: modelViewport
                    Layout.fillWidth: true; Layout.fillHeight: true
                    session: root.session
                    showGrid: root.showGrid; showAxes: root.showAxes
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
            ColumnLayout {
                anchors.centerIn: parent; width: Math.min(stage.width-40,480)
                visible: root.session.busy || root.session.preparing || !!root.session.error || !!root.session.frameError
                BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: root.session.busy || root.session.preparing; visible: running }
                Label { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; color: root.session.error || root.session.frameError ? Theme.error : Theme.text; text: root.session.error.startsWith("ANIMATION_ONLY_FBX:") ? Theme.t("此 FBX 含动画但没有模型网格。纯动画和外部动画预览将在 M07 支持。", "This FBX contains animation without mesh geometry. Animation-only and external animation preview are planned for M07.", "此 FBX 含動畫但沒有模型網格。純動畫和外部動畫預覽將在 M07 支援。") : root.session.error || root.session.frameError || (root.session.preparing ? Theme.t("正在准备视图 · 旧画面仅供参考，精确检查暂缓", "Preparing view · previous frame is reference only; exact inspection paused", "正在準備視圖 · 舊畫面僅供參考，精確檢查暫緩") : Theme.t("正在读取模型…", "Reading model…", "正在讀取模型…")) }
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
            Layout.fillWidth: true; Layout.margins: 8
            color: Theme.muted; font.pixelSize: Theme.noteText; elide: Text.ElideRight
            text: Theme.t("左键拖动旋转 · 中/右拖平移 · 滚轮缩放 · F 聚焦 · 单击检查源点", "Left drag orbit · Middle/right drag pan · Wheel zoom · F focus · Click to inspect", "左鍵拖曳旋轉 · 中/右拖平移 · 滾輪縮放 · F 聚焦 · 按一下檢查來源點")
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.line }
        ModelPointPanel { Layout.fillWidth: true; session: root.session }
    }
}
