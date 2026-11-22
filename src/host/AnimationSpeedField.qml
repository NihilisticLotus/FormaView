import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: speedInput
    required property var session
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
        border.color: parent && parent.activeFocus ? Theme.accent : Theme.line
    }
    Binding {
        target: speedInput
        property: "text"
        value: speedInput.speedText(speedInput.session.speed)
        when: !speedInput.activeFocus
        restoreMode: Binding.RestoreNone
    }
    font.family: "Consolas"
    inputMethodHints: Qt.ImhFormattedNumbersOnly
    validator: DoubleValidator {
        bottom: 0
        decimals: -1
        notation: DoubleValidator.ScientificNotation
        locale: "C"
    }
    onEditingFinished: {
        const parsed = Number(text);
        if (isFinite(parsed) && parsed > 0)
            speedInput.session.setSpeed(parsed);
        text = speedInput.speedText(speedInput.session.speed);
    }
    Accessible.name: Theme.t("播放速度（正数，回车确认）", "Playback speed (positive number, press Enter)", "播放速度（正數，按 Enter 確認）")
    ToolTip.visible: hovered
    ToolTip.delay: 600
    ToolTip.text: Accessible.name
    hoverEnabled: true
}
