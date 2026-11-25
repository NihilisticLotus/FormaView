pragma Singleton
import QtQuick
import QtCore

QtObject {
    property Settings storage: Settings { id: preferences; category: "Keyboard"; property var overrides: ({}) }
    property alias overrides: preferences.overrides
    function sync(): void { storage.sync() }
    readonly property var actions: [
        { id: "open", scope: "workspace", label: Theme.t("打开文件", "Open file", "開啟檔案"), key: "Ctrl+O" },
        { id: "address", scope: "workspace", label: Theme.t("编辑目录地址", "Edit folder address", "編輯目錄位址"), key: "Ctrl+L" },
        { id: "back", scope: "workspace", label: Theme.t("后退", "Back", "後退"), key: "Alt+Left" },
        { id: "forward", scope: "workspace", label: Theme.t("前进", "Forward", "前進"), key: "Alt+Right" },
        { id: "parent", scope: "workspace", label: Theme.t("上级目录", "Parent folder", "上級目錄"), key: "Alt+Up" },
        { id: "close", scope: "workspace", label: Theme.t("关闭文件标签", "Close file tab", "關閉檔案標籤"), key: "Ctrl+W" },
        { id: "nextTab", scope: "workspace", label: Theme.t("下一文件标签", "Next file tab", "下一檔案標籤"), key: "Ctrl+Tab" },
        { id: "prevTab", scope: "workspace", label: Theme.t("上一文件标签", "Previous file tab", "上一檔案標籤"), key: "Ctrl+Shift+Tab" },
        { id: "play", scope: "video", label: Theme.t("播放 / 暂停", "Play / pause", "播放 / 暫停"), key: "Space" },
        { id: "prevFrame", scope: "video", label: Theme.t("上一帧", "Previous frame", "上一幀"), key: "D" },
        { id: "nextFrame", scope: "video", label: Theme.t("下一帧", "Next frame", "下一幀"), key: "F" },
        { id: "markA", scope: "video", label: Theme.t("设置循环起点 A", "Set loop start A", "設置循環起點 A"), key: "Q" },
        { id: "markB", scope: "video", label: Theme.t("设置循环终点 B", "Set loop end B", "設置循環終點 B"), key: "E" },
        { id: "clearAB", scope: "video", label: Theme.t("清除片段循环", "Clear segment loop", "清除片段循環"), key: "W" },
        { id: "seekBack", scope: "video", label: Theme.t("回退", "Seek backward", "回退"), key: "Left" },
        { id: "seekForward", scope: "video", label: Theme.t("快进", "Seek forward", "快進"), key: "Right" },
        { id: "volumeUp", scope: "video", label: Theme.t("提高音量", "Volume up", "提高音量"), key: "Up" },
        { id: "volumeDown", scope: "video", label: Theme.t("降低音量", "Volume down", "降低音量"), key: "Down" },
        { id: "slower", scope: "video", label: Theme.t("降低倍速", "Decrease speed", "降低倍速"), key: "[" },
        { id: "faster", scope: "video", label: Theme.t("提高倍速", "Increase speed", "提高倍速"), key: "]" },
        { id: "normalSpeed", scope: "video", label: Theme.t("恢复原速", "Normal speed", "恢復原速"), key: "1" },
        { id: "info", scope: "video", label: Theme.t("播放信息", "Playback information", "播放資訊"), key: "Tab" },
        { id: "jump", scope: "video", label: Theme.t("跳转到时间", "Jump to time", "跳至時間"), key: "Ctrl+G" },
        { id: "shot", scope: "video", label: Theme.t("保存截图", "Save screenshot", "保存截圖"), key: "S" },
        { id: "mute", scope: "video", label: Theme.t("静音 / 取消静音", "Mute / unmute", "靜音 / 取消靜音"), key: "" },
        { id: "captions", scope: "video", label: Theme.t("显示 / 隐藏字幕", "Show / hide subtitles", "顯示 / 隱藏字幕"), key: "" },
        { id: "full", scope: "video", label: Theme.t("全屏播放", "Full screen", "全螢幕播放"), key: "" },
        { id: "simpleControls", scope: "video", label: Theme.t("切换简易 / 标准界面", "Switch simple / standard controls", "切換簡易 / 標準介面"), key: "I" },
        { id: "mini", scope: "video", label: Theme.t("置顶小窗", "Mini player", "置頂小窗"), key: "" },
        { id: "prevVideo", scope: "video", label: Theme.t("上一个视频", "Previous video", "上一部影片"), key: "" },
        { id: "nextVideo", scope: "video", label: Theme.t("下一个视频", "Next video", "下一部影片"), key: "" },
        { id: "playSettings", scope: "video", label: Theme.t("播放设置", "Playback settings", "播放設定"), key: "" },
        { id: "loadSub", scope: "video", label: Theme.t("外挂字幕", "Load subtitles", "載入字幕"), key: "" },
        { id: "imageFit", scope: "image", label: Theme.t("适合窗口", "Fit to window", "符合視窗"), key: "F" },
        { id: "actual", scope: "image", label: Theme.t("原始像素", "Actual pixels", "原始像素"), key: "Z" },
        { id: "tile", scope: "image", label: Theme.t("平铺", "Tile", "平鋪"), key: "Space" },
        { id: "rgb", scope: "image", label: Theme.t("合成通道", "Composite channels", "合成通道"), key: "C" },
        { id: "R", scope: "image", label: Theme.t("R 通道", "R channel", "R 通道"), key: "R" },
        { id: "G", scope: "image", label: Theme.t("G 通道", "G channel", "G 通道"), key: "G" },
        { id: "B", scope: "image", label: Theme.t("B 通道", "B channel", "B 通道"), key: "B" },
        { id: "A", scope: "image", label: Theme.t("A 通道", "A channel", "A 通道"), key: "A" },
        { id: "focus", scope: "model", label: Theme.t("聚焦", "Focus", "聚焦"), key: "F" },
        { id: "frameAll", scope: "model", label: Theme.t("显示全部", "Frame all", "顯示全部"), key: "Home" },
        { id: "anim", scope: "model", label: Theme.t("播放 / 暂停动画", "Play / pause animation", "播放 / 暫停動畫"), key: "Space" }
    ]
    function binding(id, custom = overrides): string {
        const row = actions.find(action => action.id === id)
        if (custom?.[id] !== undefined) return custom[id]
        if (!row?.key) return ""
        // An existing user binding takes precedence over a newly introduced default.
        const taken = Object.keys(custom || {}).some(other => {
            if (custom[other] !== row.key) return false
            const scope = actions.find(action => action.id === other)?.scope
            return scope && (scope === row.scope || scope === "workspace" || row.scope === "workspace")
        })
        return taken ? "" : row.key
    }
    function hint(id): string { return binding(id) ? "  ·  " + binding(id) : "" }
    function mapping(scope): var {
        const result = {}
        for (const row of actions)
            if (row.scope === scope && binding(row.id)) result[binding(row.id)] = row.id
        return result
    }
    function sequence(key, mods): string {
        const names = { [Qt.Key_Space]: "Space", [Qt.Key_Tab]: "Tab", [Qt.Key_Backtab]: "Tab",
            [Qt.Key_Left]: "Left", [Qt.Key_Right]: "Right", [Qt.Key_Up]: "Up", [Qt.Key_Down]: "Down",
            [Qt.Key_Home]: "Home", [Qt.Key_End]: "End", [Qt.Key_PageUp]: "PgUp", [Qt.Key_PageDown]: "PgDown" }
        let name = names[key]
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35) name = "F" + (1 + key - Qt.Key_F1)
        if (key >= 33 && key <= 126) name = String.fromCharCode(key)
        if (!name) return ""
        return ((mods & Qt.MetaModifier) ? "Meta+" : "") + ((mods & Qt.ControlModifier) ? "Ctrl+" : "")
             + ((mods & Qt.AltModifier) ? "Alt+" : "") + ((mods & Qt.ShiftModifier || key === Qt.Key_Backtab) ? "Shift+" : "") + name
    }
    function action(scope, event): string {
        return mapping(scope)[sequence(event.key, event.modifiers)] ?? ""
    }
}
