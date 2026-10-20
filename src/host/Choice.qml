pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
ComboBox {
    id: control
    property bool form: false
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
        contentItem: Text { text: control.textAt(row.index); font: control.font; color: Theme.text; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: row.highlighted ? Theme.selection : row.hovered ? Theme.hover : Theme.panel }
    }
    popup: Popup {
        y: control.height + 2; width: control.width; padding: 1
        implicitHeight: Math.min(contentItem.implicitHeight + 2, 320)
        contentItem: ListView { clip: true; implicitHeight: contentHeight; model: control.popup.visible ? control.delegateModel : null; currentIndex: control.highlightedIndex; ScrollIndicator.vertical: ScrollIndicator {} }
        background: Rectangle { color: Theme.panel; border.color: Theme.line; radius: Theme.radius }
    }
    ToolTip.visible: hovered && displayText.length > 30
    ToolTip.text: displayText
}
