import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: speedInput
    required property var session
    property bool editingDraft: false
    property bool invalid: false
    readonly property string validationMessage: Theme.t("请输入有限的正数。", "Enter a finite positive number.", "請輸入有限的正數。")
    function speedText(value: real): string {
        return value >= 0.01 && value < 1e21 && Number(value.toFixed(2)) === value ? value.toFixed(2) : value.toString();
    }
    width: 64
    enabled: session.canPlayAnimation
    horizontalAlignment: TextInput.AlignHCenter
    color: Theme.text
    background: Rectangle {
        radius: 4
        color: Theme.field
        border.width: parent && parent.activeFocus ? 2 : 1
        border.color: speedInput.invalid ? Theme.error : speedInput.activeFocus ? Theme.accent : Theme.line
    }
    Binding {
        target: speedInput
        property: "text"
        value: speedInput.speedText(speedInput.session.speed)
        when: !speedInput.editingDraft
        restoreMode: Binding.RestoreNone
    }
    Connections {
        target: speedInput.session
        function onDocumentChanged() {
            speedInput.editingDraft = false
            speedInput.invalid = false
        }
    }
    font.family: "Consolas"
    inputMethodHints: Qt.ImhFormattedNumbersOnly
    function commit(): void {
        const draft = text.trim()
        const parsed = Number(draft)
        invalid = !/^[+]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?$/i.test(draft) || !Number.isFinite(parsed) || parsed <= 0
        if (invalid) return
        session.setSpeed(parsed)
        editingDraft = false
    }
    onTextEdited: { editingDraft = true; invalid = false }
    onEditingFinished: commit()
    Keys.onEscapePressed: event => {
        editingDraft = false
        invalid = false
        event.accepted = true
    }
    Accessible.name: Theme.t("播放速度（正数，回车确认）", "Playback speed (positive number, press Enter)", "播放速度（正數，按 Enter 確認）")
    Accessible.description: invalid ? validationMessage : ""
    ToolTip.visible: (activeFocus && invalid) || hovered
    ToolTip.delay: invalid ? 0 : 600
    ToolTip.text: invalid ? validationMessage : Accessible.name
    hoverEnabled: true
}
