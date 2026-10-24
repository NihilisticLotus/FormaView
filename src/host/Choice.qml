pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
ComboBox {
    id: control
    property bool form: false
    // A fixed numeric model keeps selection stable when translated labels change.
    property var labels: []
    model: labels.length
    displayText: labels.length ? labels[currentIndex] || "" : currentText
    property var notes: []
    function noteAt(index): string { return notes[index] || "" }
    implicitHeight: control.form ? Theme.formControlHeight : Theme.controlHeight
    implicitWidth: Math.max(120, implicitContentWidth + 44)
    font.pixelSize: control.form ? Theme.formText : Theme.compactText
    leftPadding: 10; rightPadding: 30
    focusPolicy: Qt.StrongFocus
    contentItem: Text { text: control.displayText; font: control.font; color: control.enabled ? Theme.text : Theme.muted; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    indicator: AppIcon { x: control.width - width - 10; anchors.verticalCenter: parent.verticalCenter; name: "collapse"; size: 16; variant: control.enabled ? "muted" : "disabled" }
    background: Rectangle { radius: Theme.radius; color: control.down ? Theme.field : control.hovered ? Theme.hover : Theme.raised; border.color: control.activeFocus ? Theme.accent : Theme.line; border.width: control.activeFocus ? 2 : 1; opacity: control.enabled ? 1 : 0.55 }
    delegate: ItemDelegate {
        id: row
        required property int index
        width: control.width; height: Theme.rowHeight
        highlighted: control.highlightedIndex === index
        hoverEnabled: true
        Accessible.description: control.noteAt(index)
        ToolTip.visible: hovered && control.noteAt(index).length > 0
        ToolTip.text: control.noteAt(index)
        ToolTip.delay: 350
        contentItem: Text { text: control.labels.length ? control.labels[row.index] : control.textAt(row.index); font: control.font; color: Theme.text; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: row.highlighted ? Theme.selection : row.hovered ? Theme.hover : Theme.panel }
    }
    popup: Popup {
        y: control.height + 2; width: control.width; padding: 1
        implicitHeight: contentItem.implicitHeight + 2
        contentItem: Column {
            ListView { width: parent.width; height: Math.min(contentHeight, 280); clip: true; model: control.popup.visible ? control.delegateModel : null; currentIndex: control.highlightedIndex; ScrollIndicator.vertical: ScrollIndicator {} }
            Label { width: parent.width; padding: 10; visible: !!text; text: control.noteAt(control.highlightedIndex); font.pixelSize: Theme.compactText; color: Theme.text; wrapMode: Text.WordWrap; textFormat: Text.PlainText }
        }
        background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: Theme.radius }
    }
    ToolTip.visible: hovered && (displayText.length > 30 || control.noteAt(currentIndex).length > 0)
    ToolTip.text: control.noteAt(currentIndex) || displayText
}
