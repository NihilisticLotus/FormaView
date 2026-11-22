import QtQuick

Timer {
    id: gate
    required property var picker
    property bool preparing: false
    interval: 150
    repeat: false

    property FrostRequest backdrop: FrostRequest {
        host: gate.picker.parentWindow
        shown: gate.preparing || gate.picker.visible
    }
    function present(): void {
        if (preparing || picker.visible) return
        if (!picker.parentWindow || !picker.parentWindow.visible) {
            picker.open()
            return
        }
        // Present the frosted owner before Windows enters its native picker loop.
        preparing = true
        start()
        picker.parentWindow.update()
    }
    function showPicker(): void {
        stop()
        picker.open()
        preparing = false
    }
    property Connections presented: Connections {
        target: gate.running ? gate.picker.parentWindow : null
        function onFrameSwapped(): void {
            if (!gate.running) return
            gate.showPicker()
        }
    }
    onTriggered: showPicker()
}
