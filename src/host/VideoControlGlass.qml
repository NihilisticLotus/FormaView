pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: glass
    required property Item frame
    required property point panelOrigin

    // Include the blur kernel outside the dock, especially at letterbox edges.
    readonly property int marginPx: 48
    readonly property size region: Qt.size(width + 2 * marginPx, height + 2 * marginPx)
    readonly property size reduced: Qt.size(Math.max(1, Math.ceil(region.width / 4)),
                                            Math.max(1, Math.ceil(region.height / 4)))

    ShaderEffectSource {
        id: backdrop
        visible: false
        sourceItem: glass.frame
        sourceRect: Qt.rect(glass.panelOrigin.x - glass.marginPx, glass.panelOrigin.y - glass.marginPx,
                            glass.region.width, glass.region.height)
        textureSize: glass.reduced
        smooth: true
    }

    // Both passes run on the cropped, reduced resolution region. Only the
    // rounded silhouette and its highlight are drawn at display resolution.
    component BlurPass: ShaderEffect {
        required property var source
        required property vector2d sampleStep
        visible: false
        width: glass.region.width
        height: glass.region.height
        layer.enabled: true
        layer.smooth: true
        layer.textureSize: glass.reduced
        fragmentShader: "qrc:/shaders/video_blur.frag.qsb"
    }
    BlurPass {
        id: horizontal
        source: backdrop
        sampleStep: Qt.vector2d(4 / glass.region.width, 0)
    }
    BlurPass {
        id: vertical
        source: horizontal
        sampleStep: Qt.vector2d(0, 4 / glass.region.height)
    }
    ShaderEffect {
        anchors.fill: parent
        property var source: vertical
        property size panelSize: Qt.size(width, height)
        property real marginPx: glass.marginPx
        fragmentShader: "qrc:/shaders/video_glass.frag.qsb"
    }
}
