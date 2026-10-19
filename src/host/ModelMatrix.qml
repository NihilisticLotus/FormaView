pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property string title
    property var values: []
    spacing: 4
    visible: values && values.length===16
    Label { text: root.title; color: Theme.muted; font.pixelSize: Theme.noteText }
    ScrollView {
        Layout.fillWidth: true; Layout.preferredHeight: 96
        TextArea {
            objectName: "modelMatrixValues"
            readOnly: true; selectByMouse: true
            text: {
                const rows=[]
                if (root.values) for(let row=0;row<4;++row) rows.push(root.values.slice(row*4,row*4+4).map(String).join("   "))
                return rows.join("\n")
            }
            wrapMode: TextEdit.NoWrap; font.family: "Consolas"; font.pixelSize: 12
            color: Theme.text; selectionColor: Theme.selection
            background: Rectangle { color: Theme.field; radius: Theme.radius; border.color: Theme.line }
            Accessible.name: root.title
        }
    }
}
