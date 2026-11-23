pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: root
    property string kind: ""
    property bool folder: false
    property int size: 24
    readonly property string category: folder ? "" : kind === "image" ? "image"
        : kind === "video" ? "video" : ["model", "convert", "cache", "animation", "skeleton"].includes(kind) ? "model" : ""
    readonly property int rasterSize: {
        const wanted = size * Screen.devicePixelRatio
        for (const value of [16,20,24,32,40,48,64,128,256,512])
            if (value >= wanted) return value
        return 512
    }
    implicitWidth: size
    implicitHeight: size
    Accessible.ignored: true
    Image {
        objectName: "fileTypeRaster"
        anchors.fill: parent
        visible: !!root.category
        source: root.category ? "qrc:/formaview/icons/file-types/formaview-" + root.category + "-" + root.rasterSize + ".png" : ""
        sourceSize: Qt.size(root.rasterSize, root.rasterSize)
        fillMode: Image.PreserveAspectFit
        Accessible.ignored: true
    }
    AppIcon {
        anchors.fill: parent
        visible: !root.category
        name: root.folder ? "folder" : "file"
        size: root.size
        variant: "muted"
    }
}
