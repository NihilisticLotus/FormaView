pragma ComponentBehavior: Bound
import QtQuick
import FormaView.Models

FocusScope {
    id: root
    required property var session
    property bool uv: false
    property bool showGrid: true
    property bool showAxes: true
    property bool wireframe: false
    property var initialCamera: ({})
    property bool ready: false
    signal cameraSaved(var state)
    function fit(selected: bool): void { viewport.fit(selected) }
    Component.onCompleted: { viewport.restoreCamera(root.initialCamera); root.ready=true }
    ModelView {
        id: viewport
        objectName: root.uv ? "uvViewport" : "modelViewport"
        anchors.fill: parent
        session: root.session
        representation: root.uv ? 1 : 0
        showGrid: !root.uv && root.showGrid
        showAxes: !root.uv && root.showAxes
        wireframe: !root.uv && root.wireframe
        onCameraChanged: state => { if (root.ready) root.cameraSaved(state) }
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        Accessible.name: root.uv ? Theme.t("UV 预览", "UV preview", "UV 預覽") : Theme.t("模型预览", "Model preview", "模型預覽")
        Keys.onPressed: event => {
            if (event.key === Qt.Key_F) { viewport.fit(true); event.accepted = true }
            else if (event.key === Qt.Key_Home) { viewport.fit(); event.accepted = true }
            else if (event.key === Qt.Key_Space && !root.uv && root.session && root.session.animated) { root.session.togglePlay(); event.accepted = true }
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            property point last
            property point pressedAt
            property bool dragging: false
            onPressed: mouse => { viewport.forceActiveFocus(); last = Qt.point(mouse.x,mouse.y); pressedAt = last; dragging=false }
            onPositionChanged: mouse => {
                if (!pressed) return
                if (!dragging && Math.hypot(mouse.x-pressedAt.x,mouse.y-pressedAt.y)<4) return
                const dx=mouse.x-last.x, dy=mouse.y-last.y
                dragging=true
                if (root.uv || (pressedButtons & (Qt.MiddleButton|Qt.RightButton))) viewport.panBy(dx,dy)
                else if (pressedButtons & Qt.LeftButton) viewport.orbit(dx,dy)
                last=Qt.point(mouse.x,mouse.y)
            }
            onReleased: mouse => {
                if (mouse.button === Qt.LeftButton && !dragging && !(mouse.modifiers & Qt.AltModifier))
                    viewport.pickAt(mouse.x,mouse.y)
            }
            onDoubleClicked: viewport.fit(true)
            onWheel: wheel => { viewport.zoomBy(Math.pow(1.0015,wheel.angleDelta.y)); wheel.accepted=true }
        }
    }
}
