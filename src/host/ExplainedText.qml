pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
// A localized explanation with the producer's stable technical text below it.
// kind: "loss" (code+detail), "error" or "note" (raw string).
ColumnLayout {
    id: root
    property string kind: "error"
    property string code
    property string detail
    property string raw
    property bool dismissible: false
    signal dismissed()
    property color tone: Theme.error
    readonly property string explanation: kind === "loss" ? Explain.lossText(code, detail)
                                        : kind === "note" ? Explain.noteText(raw) : Explain.errorText(raw)
    readonly property string technical: kind === "loss" ? Explain.lossTechnical(code, detail) : Explain.errorTechnical(raw)
    readonly property bool showTechnical: technical.length > 0 && technical !== explanation
    visible: explanation.length > 0
    spacing: 2
    RowLayout {
        Layout.fillWidth: true
        Label { objectName: "explainedText"; Layout.fillWidth: true; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: root.tone; text: root.explanation }
        Tool {
            objectName: "dismissExplanation"; visible: root.dismissible; quiet: true
            text: Theme.t("关闭提示", "Dismiss", "關閉提示")
            onClicked: root.dismissed()
        }
    }
    TextEdit {
        objectName: "explainedTechnical"
        Layout.fillWidth: true; visible: root.showTechnical
        readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere; textFormat: TextEdit.PlainText
        color: Theme.muted; selectionColor: Theme.action; font.pixelSize: Theme.noteText
        text: Theme.t("技术信息：", "Technical: ", "技術資訊：") + root.technical
        Accessible.role: Accessible.StaticText; Accessible.name: text
    }
}
