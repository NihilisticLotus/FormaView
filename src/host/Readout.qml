import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
GridLayout {
    id: root
    required property string label
    required property string value
    property bool inlineValue: false
    columns: inlineValue ? 2 : 1
    rowSpacing: 4; columnSpacing: 16
    Label { Layout.fillWidth: !root.inlineValue; Layout.preferredWidth: root.inlineValue ? 88 : -1; Layout.alignment: Qt.AlignTop; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; text: root.label; color: Theme.muted; font.pixelSize: 12 }
    TextEdit {
        objectName: "readoutValue"
        Layout.fillWidth: true
        Layout.preferredWidth: 0
        text: root.value
        textFormat: TextEdit.PlainText
        readOnly: true
        selectByMouse: true
        activeFocusOnTab: true
        color: Theme.text
        selectedTextColor: Theme.text
        selectionColor: Theme.selection
        font.pixelSize: root.inlineValue ? Theme.compactText : 14
        wrapMode: TextEdit.WrapAnywhere
        Accessible.name: root.label
        Rectangle { anchors.fill: parent; anchors.margins: -2; z: -1; color: "transparent"; border.width: 2; border.color: parent.activeFocus ? Theme.accent : "transparent" }
    }
}
