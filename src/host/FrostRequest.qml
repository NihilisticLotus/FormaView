import QtQuick

QtObject {
    id: request
    required property Window host
    required property bool shown
    property Window appliedHost: null

    function release(): void {
        if (appliedHost)
            appliedHost.frostDepth--
        appliedHost = null
    }
    function updateHost(): void {
        const next = shown && host && typeof host.frostDepth === "number" ? host : null
        if (next === appliedHost) return
        release()
        if (next) {
            next.frostDepth++
            appliedHost = next
        }
    }
    onHostChanged: updateHost()
    onShownChanged: updateHost()
    Component.onDestruction: release()
}
