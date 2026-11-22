import QtQuick
import QtQuick.Controls.impl
Item {
    id: root
    property string name: ""
    property string variant: "normal"
    property int size: 18
    implicitWidth: size; implicitHeight: size
    Accessible.ignored: true
    IconImage {
        color: !Theme.light ? "transparent" : root.name === "brand-diamond" ? Theme.accent : root.variant === "on-action" ? "white" : root.variant === "normal" ? Theme.text : Theme.muted
        anchors.fill: parent
        source: root.name ? "qrc:/formaview/icons/" + root.variant + "/" + root.name + ".svg" : ""
        sourceSize: Qt.size(root.size * Screen.devicePixelRatio, root.size * Screen.devicePixelRatio)
        fillMode: Image.PreserveAspectFit
        Accessible.ignored: true
    }
}
