pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var custom
    signal edited(var value)
    property string recording: ""
    property string feedback: ""
    readonly property var groups: [
        ["workspace", Theme.t("工作区", "Workspace", "工作區")],
        ["video", Theme.t("视频播放", "Video playback", "影片播放")],
        ["image", Theme.t("图片检查", "Image inspection", "影像檢查")],
        ["model", Theme.t("模型预览", "Model preview", "模型預覽")]]
    spacing: 12
    function record(row, event): void {
        event.accepted = true
        if (event.isAutoRepeat) return
        if (event.key === Qt.Key_Escape) { recording = ""; feedback = ""; return }
        if ([Qt.Key_Shift, Qt.Key_Control, Qt.Key_Meta, Qt.Key_Alt].includes(event.key)) return
        const clear = event.key === Qt.Key_Backspace && !event.modifiers
        const key = clear ? "" : KeyBindings.sequence(event.key, event.modifiers)
        if (!clear && !key) {
            feedback = Theme.t("请选择其他按键", "Choose another key", "請選擇其他按鍵")
            return
        }
        const other = key && KeyBindings.actions.find(action => action.id !== row.id
            && (action.scope === row.scope || row.scope === "workspace" || action.scope === "workspace")
            && KeyBindings.binding(action.id, custom) === key)
        if (other) {
            feedback = Theme.t("已分配给", "Already assigned to", "已指派給") + " " + other.label
            return
        }
        const next = Object.assign({}, custom)
        if (key === row.key) delete next[row.id]
        else next[row.id] = key
        edited(next)
        recording = ""
        feedback = ""
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted
        text: Theme.t("点击键位后按下新组合。Backspace 清空，Esc 取消。", "Select a binding and press the new keys. Backspace clears, Esc cancels.", "點選鍵位後按下新組合。Backspace 清除，Esc 取消。")
    }
    Tool {
        objectName: "resetShortcutDefaults"
        text: Theme.t("恢复默认快捷键", "Restore default shortcuts", "還原預設快速鍵")
        enabled: Object.keys(root.custom).length > 0
        onClicked: { root.recording = ""; root.feedback = ""; root.edited({}) }
    }
    Repeater {
        model: root.groups
        delegate: SettingsGroup {
            id: group
            required property var modelData
            title: modelData[1]
            Repeater {
                model: KeyBindings.actions.filter(row => row.scope === group.modelData[0])
                delegate: SettingsRow {
                    id: row
                    required property var modelData
                    label: modelData.label
                    description: root.recording === modelData.id ? root.feedback : ""
                    Tool {
                        objectName: "shortcut-" + row.modelData.id
                        Layout.preferredWidth: 150
                        readonly property bool recording: root.recording === row.modelData.id
                        text: recording ? Theme.t("按下新快捷键…", "Press keys…", "按下新快速鍵…") :
                              KeyBindings.binding(row.modelData.id, root.custom) || Theme.t("未绑定", "Unassigned", "未綁定")
                        Accessible.name: row.label + " " + text
                        onClicked: { forceActiveFocus(); root.feedback = ""; root.recording = row.modelData.id }
                        onActiveFocusChanged: if (!activeFocus && recording) root.recording = ""
                        Keys.onShortcutOverride: event => { if (recording) event.accepted = true }
                        Keys.onPressed: event => {
                            if (recording) root.record(row.modelData, event)
                            else event.accepted = false
                        }
                    }
                }
            }
        }
    }
    Label {
        Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap
        text: Theme.t("列表导航：方向键移动，Home / End 跳到首尾，Page Up / Down 翻页，Enter 打开，Ctrl+A 全选。Esc 返回或关闭弹层。",
                      "List navigation: arrows move, Home / End go to the ends, Page Up / Down page, Enter opens and Ctrl+A selects all. Esc goes back or closes a popup.",
                      "清單導覽：方向鍵移動，Home / End 跳至首尾，Page Up / Down 翻頁，Enter 開啟，Ctrl+A 全選。Esc 返回或關閉彈層。")
    }
}
