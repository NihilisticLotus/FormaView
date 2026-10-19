pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FormaView.Images

ColumnLayout {
    id: root
    required property var controller
    readonly property var session: controller.image
    readonly property var mapping: session.info.mapping || ["", "", "", ""]
    property bool fixedSample: false
    property bool loadingNotice: false
    Timer { id: loadingDelay; interval: 300; onTriggered: root.loadingNotice = root.session.busy }
    Connections {
        target: root.session
        function onChanged() {
            if (!root.session.busy) { loadingDelay.stop(); root.loadingNotice = false }
            else if (!loadingDelay.running && !root.loadingNotice) loadingDelay.start()
        }
    }
    Component.onCompleted: if (session.busy) loadingDelay.start()
    spacing: 4
    Flow {
        Layout.fillWidth: true; Layout.preferredHeight: implicitHeight; Layout.margins: 8; spacing: 4
        Repeater {
            model: ["RGB · C", "R", "G", "B", "A"]
            Tool {
                required property string modelData
                required property int index
                objectName: "imageChannel" + index
                text: modelData; checked: root.session.display.channel === index - 1
                enabled: index === 0 ? !!root.mapping[0] && !!root.mapping[1] && !!root.mapping[2] : !!root.mapping[index - 1]
                hint: enabled ? modelData : Theme.t("缺少源通道；可在检查区映射", "Source channel absent; map in Inspect", "缺少來源通道；可在檢查區映射")
                onClicked: root.session.setting("channel", index - 1)
            }
        }
        Tool {
            text: Theme.t("Alpha 合成", "Composite alpha", "Alpha 合成")
            checkable: true; checked: root.session.display.composite
            enabled: root.session.display.channel === -1 && !!root.mapping[3] && root.session.display.alpha >= 2
            onClicked: root.session.setting("composite", checked)
        }
        Tool { text: Theme.t("适合 F", "Fit F", "適合 F"); onClicked: view.fit() }
        Tool { text: "100% Z"; onClicked: view.actual() }
        Tool { text: Theme.t("平铺 Space", "Tile Space", "平鋪 Space"); checked: root.session.display.tiled; onClicked: root.session.setting("tiled", !checked) }
        ExposureSpinBox {
            objectName: "imageExposure"
            visible: root.session.channels.some(c => ["half", "float", "double"].includes(c.type))
            value: Math.round(root.session.display.exposure * 100)
            enabled: root.session.display.colour && root.session.display.channel < 0
            Accessible.name: Theme.t("曝光 EV · 仅显示", "Exposure EV · Display only", "曝光 EV · 僅顯示")
            ToolTip.visible: hovered; ToolTip.text: Accessible.name
            onValueModified: root.session.setting("exposure", realValue)
        }
        Tool {
            iconName: "more"; quiet: true; hint: Theme.t("更多", "More", "更多"); onClicked: more.open()
            AppPopupMenu {
                id: more; y: parent.height
                AppMenuItem { text: Theme.t("放大", "Zoom in", "放大"); onTriggered: view.zoomAt(1.25, view.width / 2, view.height / 2) }
                AppMenuItem { text: Theme.t("缩小", "Zoom out", "縮小"); onTriggered: view.zoomAt(.8, view.width / 2, view.height / 2) }
                AppMenuItem { text: Theme.t("向左平移", "Pan left", "向左平移"); onTriggered: view.pan = Qt.point(view.pan.x - 64, view.pan.y) }
                AppMenuItem { text: Theme.t("向右平移", "Pan right", "向右平移"); onTriggered: view.pan = Qt.point(view.pan.x + 64, view.pan.y) }
                AppMenuItem { text: Theme.t("向上平移", "Pan up", "向上平移"); onTriggered: view.pan = Qt.point(view.pan.x, view.pan.y - 64) }
                AppMenuItem { text: Theme.t("向下平移", "Pan down", "向下平移"); onTriggered: view.pan = Qt.point(view.pan.x, view.pan.y + 64) }
                AppMenuItem { text: Theme.t("使用 displayWindow", "Use displayWindow", "使用 displayWindow"); checkable: true; checked: root.session.display.displayWindow; onTriggered: { root.session.setting("displayWindow", checked); view.fit() } }
                AppPopupMenu {
                    title: Theme.t("透明合成背景", "Alpha composite background", "透明合成背景")
                    AppMenuItem { text: Theme.t("棋盘格", "Checkerboard", "棋盤格"); checkable: true; checked: root.session.display.background === 0; onTriggered: root.session.setting("background", 0) }
                    AppMenuItem { text: Theme.t("黑色", "Black", "黑色"); checkable: true; checked: root.session.display.background === 1; onTriggered: root.session.setting("background", 1) }
                    AppMenuItem { text: Theme.t("白色", "White", "白色"); checkable: true; checked: root.session.display.background === 2; onTriggered: root.session.setting("background", 2) }
                    AppMenuItem { text: Theme.t("中灰", "Mid grey", "中灰"); checkable: true; checked: root.session.display.background === 3; onTriggered: root.session.setting("background", 3) }
                }
            }
        }
    }
    Item {
        Layout.fillWidth: true; Layout.fillHeight: true
        ImageView {
            id: view; objectName: "imageViewport"; anchors.fill: parent; session: root.session; activeFocusOnTab: true
            Accessible.role: Accessible.Canvas
            Accessible.name: Theme.t("图片视口；F 适合，Z 原始像素，空格平铺，C/R/G/B/A 通道", "Image viewport; F fit, Z actual pixels, Space tile, C/R/G/B/A channels", "影像視口；F 適合，Z 原始像素，空格平鋪，C/R/G/B/A 通道")
            Keys.onPressed: event => {
                if (event.isAutoRepeat || event.modifiers !== Qt.NoModifier || more.visible) return
                if (event.key === Qt.Key_F) view.fit()
                else if (event.key === Qt.Key_Z) view.actual()
                else if (event.key === Qt.Key_Space) root.session.setting("tiled", !root.session.display.tiled)
                else if (event.key === Qt.Key_C) root.session.setting("channel", -1)
                else if (event.key === Qt.Key_R) root.session.setting("channel", 0)
                else if (event.key === Qt.Key_G) root.session.setting("channel", 1)
                else if (event.key === Qt.Key_B) root.session.setting("channel", 2)
                else if (event.key === Qt.Key_A) root.session.setting("channel", 3)
                else if (event.key === Qt.Key_Escape && root.fixedSample) root.fixedSample = false
                else return
                event.accepted = true
            }
        }
        Rectangle { anchors.fill: parent; color: "transparent"; border.width: view.activeFocus ? 2 : 0; border.color: Theme.accent; Accessible.ignored: true }
        MouseArea {
            anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            property point start
            property point oldPan
            onPressed: mouse => { view.forceActiveFocus(); start = Qt.point(mouse.x, mouse.y); oldPan = view.pan }
            onClicked: mouse => { if (mouse.button === Qt.LeftButton) { view.pick(mouse.x, mouse.y); root.fixedSample = !root.fixedSample } }
            onPositionChanged: mouse => {
                if (pressedButtons & Qt.MiddleButton) view.pan = Qt.point(oldPan.x + mouse.x - start.x, oldPan.y + mouse.y - start.y)
                else if (!root.fixedSample) view.pick(mouse.x, mouse.y)
            }
            onWheel: wheel => { view.zoomAt(Math.pow(1.0015, wheel.angleDelta.y), wheel.x, wheel.y); wheel.accepted = true }
        }
        Rectangle {
            objectName: "imageLoadingNotice"
            anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 12
            visible: root.loadingNotice || !!root.session.error
            width: root.session.error ? Math.min(560, parent.width - 24) : Math.min(notice.implicitWidth + 24, parent.width - 24)
            height: notice.implicitHeight + 16
            color: Theme.panel; radius: Theme.radius; border.color: root.session.error ? Theme.error : Theme.line
            Label {
                id: notice; x: 12; y: 8; width: parent.width - 24
                textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; color: root.session.error ? Theme.error : Theme.muted
                text: root.session.error ? root.session.error + "\n" + Theme.t("仍显示：", "Still showing: ", "仍顯示：") + root.session.path : Theme.t("正在加载…", "Loading…", "正在載入…")
            }
        }
        Label { textFormat: Text.PlainText; anchors.centerIn: parent; width: parent.width - 40; visible: !!view.renderError; wrapMode: Text.WordWrap; color: Theme.error; text: Theme.t("图片无法在 GPU 上显示；源文件未更改。", "The GPU could not display this image; the source is unchanged.", "GPU 無法顯示此影像；來源檔案未變更。") + "\n" + view.renderError }
        Rectangle {
            anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 10
            width: Math.min(parent.width - 20, hud.implicitWidth + 12); height: hud.implicitHeight + 8; color: "#dd222326"; radius: Theme.radius
            Label { textFormat: Text.PlainText; id: hud; x: 6; y: 4; width: parent.width - 12; color: Theme.text; wrapMode: Text.WordWrap; text: (view.pixelScale * 100).toFixed(1) + "% · " + (root.session.display.channel < 0 ? "RGB" : ["R", "G", "B", "A"][root.session.display.channel]) + (root.session.pixel.valid ? " · (" + root.session.pixel.x + ", " + root.session.pixel.y + ")" + (root.session.display.tiled ? " · tile (" + root.session.pixel.tileX + ", " + root.session.pixel.tileY + ")" : "") : "") + (root.fixedSample ? Theme.t(" · 固定取样", " · Pinned sample", " · 固定取樣") : "") }
        }
    }
    SectionHeader { Layout.fillWidth: true; id: pixelToggle; objectName: "pixelToggle"; checkable: true; text: Theme.t("像素检查 · 原值 / 显示值", "Pixel inspection · Source / Display", "像素檢查 · 原值 / 顯示值") }
    // Keep this lightweight readout alive: a deferred inline Component can lose
    // its creation context during QML Preview updates and leave an empty panel.
    PixelPanel {
        Layout.fillWidth: true; Layout.fillHeight: false; Layout.preferredHeight: implicitHeight
        Layout.minimumHeight: 0; Layout.maximumHeight: Math.max(0, root.height * .35)
        visible: pixelToggle.checked
        controller: root.controller
    }
}
