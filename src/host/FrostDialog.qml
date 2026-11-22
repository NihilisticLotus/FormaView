import QtQuick
import QtQuick.Controls.Basic

Dialog {
    id: root
    property FrostRequest backdropRequest: FrostRequest {
        host: root.parent ? root.parent.Window.window : null
        shown: root.visible
    }
}
