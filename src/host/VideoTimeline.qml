pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root
    required property real duration
    required property real position
    required property real pointA
    required property real pointB
    required property var formatTime
    signal seekRequested(real seconds)
    signal endpointMoved(string point, real seconds)

    readonly property bool interacting: scrub.pressed || dragActive
    readonly property real shownPosition: scrub.engaged ? scrub.value : position
    property bool dragActive: false
    implicitHeight: 34

    Slider {
        id: scrub
        objectName: "videoTimeline"
        anchors.fill: parent
        leftPadding: 8
        rightPadding: 8
        Accessible.name: qsTr("播放位置")
        from: 0
        to: Math.max(0.001, root.duration)
        enabled: root.duration > 0
        stepSize: 0.1
        property bool engaged: false
        Binding on value {
            when: !scrub.engaged
            value: root.position
            restoreMode: Binding.RestoreNone
        }
        onPressedChanged: {
            if (pressed)
                engaged = true
            else if (engaged) {
                root.seekRequested(value)
                engaged = false
            }
        }
        onMoved: if (!engaged) root.seekRequested(value)
        background: Rectangle {
            id: track
            x: scrub.leftPadding
            width: scrub.availableWidth
            y: Math.round(scrub.height / 2) - 2
            height: 4
            radius: 2
            color: "#7e8088"
            Rectangle {
                width: parent.width * scrub.visualPosition
                height: parent.height
                radius: 2
                color: "#ffa764"
            }
            Rectangle {
                visible: root.pointA >= 0 && root.pointB > root.pointA
                x: parent.width * root.pointA / scrub.to
                width: parent.width * (root.pointB - root.pointA) / scrub.to
                y: parent.height
                height: 3
                color: "#ffcc9e"
            }
        }
        handle: Rectangle {
            implicitWidth: 15
            implicitHeight: 15
            x: scrub.leftPadding + scrub.availableWidth * scrub.visualPosition - width / 2
            y: scrub.height / 2 - height / 2
            radius: width / 2
            color: "#ffad67"
            border.color: scrub.visualFocus ? "#ffffff" : "#ffe4bf"
        }
    }
    HoverHandler {
        id: hover
        enabled: scrub.enabled
    }
    Rectangle {
        id: tooltip
        visible: (hover.hovered || scrub.pressed) && !root.dragActive
        z: 2
        x: Math.min(root.width - width, Math.max(0, (scrub.pressed ? scrub.handle.x + scrub.handle.width / 2 : hover.point.position.x) - width / 2))
        y: -height - 2
        width: hoverTime.implicitWidth + 14
        height: hoverTime.implicitHeight + 8
        radius: 5
        color: "#ed202228"
        border.color: Theme.line
        Label {
            id: hoverTime
            anchors.centerIn: parent
            font.family: "Consolas"
            color: Theme.text
            text: root.formatTime(scrub.pressed ? scrub.value :
                Math.max(0, Math.min(1, (hover.point.position.x - scrub.leftPadding) / Math.max(1, scrub.availableWidth))) * root.duration)
        }
    }
    Repeater {
        model: ["A", "B"]
        delegate: Item {
            id: endpoint
            required property string modelData
            readonly property real value: modelData === "A" ? root.pointA : root.pointB
            visible: value >= 0
            z: pressedHandler.pressed ? 4 : 3
            width: 22
            height: 20
            x: scrub.leftPadding + scrub.availableWidth * Math.max(0, value) / scrub.to - width / 2
            y: scrub.height / 2 + 2
            activeFocusOnTab: visible
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("循环端点 %1").arg(modelData)
            Rectangle {
                anchors.fill: parent
                radius: 5
                color: "#f2b686"
                border.color: endpoint.activeFocus ? "white" : "#201813"
                Text {
                    anchors.centerIn: parent
                    text: endpoint.modelData
                    color: "#201813"
                    font.pixelSize: 11
                    font.bold: true
                }
            }
            Keys.onPressed: event => {
                if (event.key !== Qt.Key_Left && event.key !== Qt.Key_Right)
                    return
                root.endpointMoved(endpoint.modelData, endpoint.value + (event.key === Qt.Key_Left ? -0.1 : 0.1))
                event.accepted = true
            }
            MouseArea {
                id: pressedHandler
                anchors.fill: parent
                cursorShape: Qt.SizeHorCursor
                onPressed: {
                    endpoint.forceActiveFocus()
                    root.dragActive = true
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const localPoint = pressedHandler.mapToItem(scrub, mouse.x, mouse.y)
                    root.endpointMoved(endpoint.modelData, (localPoint.x - scrub.leftPadding) * root.duration / Math.max(1, scrub.availableWidth))
                }
                onReleased: root.dragActive = false
                onCanceled: root.dragActive = false
            }
        }
    }
}
