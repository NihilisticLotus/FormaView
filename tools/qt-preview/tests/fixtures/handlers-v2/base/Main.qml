import QtQuick
Window {
    id: root
    width: 600; height: 400; visible: true
    property int plainCount: 0
    property int arrowCount: 0
    property int connectionsCount: 0
    property int externalBlockCount: 0
    property int externalArrowCount: 0
    SignalTarget {
        id: signalTarget
        objectName: "signal-target"
        onPlainPing: { root.plainCount += 1 }
        onArrowPing: value => { root.arrowCount += 1 }
    }
    ExternalObservers {
        id: observers
        objectName: "external-observers"
        targetObject: signalTarget
        onBlockObserved: { root.externalBlockCount += 1 }
        onArrowObserved: value => { root.externalArrowCount += 1 }
        onFunctionObserved: { root.connectionsCount += 1 }
    }
}
