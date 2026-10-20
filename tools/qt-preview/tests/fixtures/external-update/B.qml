import QtQuick
Item {
    id: root
    property string epoch: "B"
    property int extraMember47: 47
    required property QtObject targetObject
    signal blockObserved(int value)
    signal arrowObserved(int value)
    signal functionObserved(int value)
    Connections {
        objectName: "external-block"
        target: root.targetObject
        onPulse: { root.blockObserved(value * 10) }
    }
    Connections {
        objectName: "external-arrow"
        target: root.targetObject
        onPulse: value => { root.arrowObserved(value * 10) }
    }
    Connections {
        objectName: "external-function"
        target: root.targetObject
        function onPulse(value) { root.functionObserved(value * 10) }
    }
}
