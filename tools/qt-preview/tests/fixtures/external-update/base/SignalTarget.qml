import QtQuick
Rectangle {
    id: root
    width: 200; height: 200
    property string epoch: "A"
    property int internalCount: 0
    property int keysCount: 0
    property string lastPulseEpoch: ""
    property string lastKeyEpoch: ""
    signal plainPing(int value)
    signal arrowPing(int value)
    signal pulse(int value)
    onPulse: { root.internalCount += 1; root.lastPulseEpoch = "A" }
    Keys.onEscapePressed: event => { root.keysCount += 1; root.lastKeyEpoch = "A"; event.accepted = true }
}
