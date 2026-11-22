pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects

Rectangle {
    id: panel
    radius: 16
    color: Theme.glass
    border.color: Theme.glassEdge
    RectangularShadow {
        anchors.fill: parent; z: -1
        radius: panel.radius; blur: 16; offset.y: 4
        color: Theme.light ? "#18233848" : "#3309121b"
    }
    Rectangle {
        anchors.fill: parent; anchors.margins: 1; radius: panel.radius - 1
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.light ? "#55f9ffff" : "#125f92ba" }
            GradientStop { position: 0.48; color: "transparent" }
            GradientStop { position: 1; color: Theme.light ? "#16fff1dc" : "#156b7a82" }
        }
    }
    Rectangle {
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: panel.radius; anchors.rightMargin: panel.radius; height: 1
        color: Theme.light ? "#e5ffffff" : "#8293b0c3"
    }
}
