import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
ColumnLayout {
    id: root
    required property string label
    required property string value
    spacing: 4
    Label { Layout.fillWidth: true; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; text: root.label; color: Theme.muted; font.pixelSize: 12 }
    TextEdit {
        objectName: "readoutValue"
        Layout.fillWidth: true
        text: root.value
        textFormat: TextEdit.PlainText
        readOnly: true
        selectByMouse: true
        activeFocusOnTab: true
        color: Theme.text
        selectedTextColor: Theme.text
        selectionColor: Theme.selection
        font.pixelSize: 14
        wrapMode: TextEdit.WrapAnywhere
        Accessible.name: root.label
        Rectangle { anchors.fill: parent; anchors.margins: -2; z: -1; color: "transparent"; border.width: 2; border.color: parent.activeFocus ? Theme.accent : "transparent" }
    }
}
