import QtQuick
Item {
    id: root
    property string name: ""
    property string variant: "normal"
    property int size: 18
    implicitWidth: size; implicitHeight: size
    Accessible.ignored: true
    Image {
        anchors.fill: parent
        source: root.name ? "qrc:/formaview/icons/" + root.variant + "/" + root.name + ".svg" : ""
        sourceSize: Qt.size(root.size * Screen.devicePixelRatio, root.size * Screen.devicePixelRatio)
        fillMode: Image.PreserveAspectFit
        Accessible.ignored: true
    }
}
