import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
ColumnLayout {
    id: root
    property string title: ""
    default property alias rows: body.data
    Layout.fillWidth: true
    spacing: 16
    data: [Label { text: root.title; font.pixelSize: Theme.formHeading; font.bold: true; color: Theme.text },
    Rectangle {
        Layout.fillWidth: true; implicitHeight: body.implicitHeight + 12
        radius: Theme.groupRadius; gradient: Gradient { GradientStop { position: 0; color: Theme.raised } GradientStop { position: 1; color: Theme.panel } }
        border.color: Theme.line
        ColumnLayout { id: body; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6; anchors.leftMargin: 20; anchors.rightMargin: 20; spacing: 0 }
    }]
}
