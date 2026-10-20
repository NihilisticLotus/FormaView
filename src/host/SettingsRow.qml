import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
RowLayout {
    id: root
    property string label: ""
    property string description: ""
    Layout.fillWidth: true
    Layout.minimumHeight: description ? 72 : 64
    spacing: 24
    ColumnLayout {
        Layout.fillWidth: true; spacing: 4
        Label { Layout.fillWidth: true; text: root.label; color: Theme.text; font.pixelSize: Theme.formText; wrapMode: Text.WordWrap }
        Label { visible: !!root.description; Layout.fillWidth: true; text: root.description; color: Theme.muted; font.pixelSize: Theme.noteText; wrapMode: Text.WordWrap }
    }
}
