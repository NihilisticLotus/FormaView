import QtQuick
Item {
    id: root
    required property QtObject targetObject
    signal blockObserved(int value)
    signal arrowObserved(int value)
    signal functionObserved(int value)
    Connections {
        objectName: "external-block"
        target: root.targetObject
        onPulse: { root.blockObserved(value) }
    }
    Connections {
        objectName: "external-arrow"
        target: root.targetObject
        onPulse: value => { root.arrowObserved(value) }
    }
    Connections {
        objectName: "external-function"
        target: root.targetObject
        function onPulse(value) { root.functionObserved(value) }
    }
}
