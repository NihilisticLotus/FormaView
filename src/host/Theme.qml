pragma Singleton
import QtQuick
QtObject {
    property bool light: false
    readonly property color glass: light ? "#1fffffff" : "#45192633"
    readonly property color glassEdge: light ? "#bcffffff" : "#598da4b6"
    readonly property color thumbnail: light ? "#dee7ee" : field
    property bool reduceMotion: false
    property bool compact: false
    readonly property int bodyText: 13
    readonly property int compactText: 13
    readonly property int noteText: 12
    readonly property int formText: compact ? 14 : 16
    readonly property int formHeading: 18
    readonly property int formControlHeight: compact ? 34 : 40
    readonly property int controlHeight: compact ? 28 : 32
    readonly property int rowHeight: compact ? 32 : 36
    readonly property int fileRowHeight: compact ? 48 : 60
    readonly property int topBarHeight: 60
    readonly property int statusHeight: 28
    readonly property int panelGap: 8
    readonly property int filesWidth: 236
    readonly property int inspectorWidth: 340
    readonly property int narrowInspectorWidth: 300
    readonly property int railWidth: 40
    readonly property int radius: 6
    readonly property int groupRadius: 16
    readonly property int dialogRadius: 14
    readonly property color hover: light ? "#e1ebf4" : "#405568"
    property string language: "zh-CN"
    readonly property color base: light ? "#e8eff4" : "#253443"
    readonly property color sidebar: light ? "#e1eaf0" : "#273b4d"
    readonly property color panel: light ? "#f3f7fa" : "#2e4253"
    readonly property color raised: light ? "#f9fbfd" : "#374d60"
    readonly property color field: light ? "#f3f7fb" : "#2e4253"
    readonly property color line: light ? "#c4d2df" : "#526779"
    readonly property color text: light ? "#1e3044" : "#eef4fa"
    readonly property color muted: light ? "#4d6278" : "#becddb"
    readonly property color tertiary: light ? "#586d81" : "#aabccb"
    readonly property color accent: light ? "#2767a3" : "#a6d2ef"
    readonly property color action: light ? "#275f99" : "#33688e"
    readonly property color actionHover: light ? "#204f82" : "#2d5b7e"
    readonly property color actionPressed: light ? "#193f68" : "#284f6d"
    readonly property color selection: light ? "#d5e6f6" : "#415e75"
    readonly property color error: light ? "#9a322f" : "#f87171"
    readonly property color success: light ? "#22684c" : "#4ade80"
    readonly property color warning: light ? "#805419" : "#fbbf24"
    function revealIn(scroll: var, focused: var): void {
        const view = scroll.contentItem as Flickable
        if (!view || !focused) return
        if (focused.hasOwnProperty("focusReason") && focused.focusReason === Qt.MouseFocusReason) return
        let ancestor = focused
        while (ancestor && ancestor !== view.contentItem) ancestor = ancestor.parent
        if (!ancestor) return
        const position = focused.mapToItem(view.contentItem, 0, 0)
        let next = view.contentY
        if (position.y < next + 6) next = position.y - 6
        else if (position.y + focused.height > next + view.height - 6) next = position.y + focused.height - view.height + 6
        view.contentY = Math.max(0, Math.min(Math.max(0, view.contentHeight - view.height), next))
    }
    function t(cn: string, en: string, tw: string): string { return language === "en" ? en : language === "zh-TW" ? tw : cn }
    function state(s: string): string {
        switch (s) {
        case "queued": return t("排队", "Queued", "排隊")
        case "running": return t("运行中", "Running", "執行中")
        case "pausing": return t("正在暂停", "Pausing", "正在暫停")
        case "paused": return t("已暂停", "Paused", "已暫停")
        case "cancelling": return t("取消中", "Cancelling", "取消中")
        case "cancelled": return t("已取消", "Cancelled", "已取消")
        case "succeeded": return t("已完成", "Completed", "已完成")
        case "failed": return t("失败", "Failed", "失敗")
        case "stale": return t("已替换", "Replaced", "已取代")
        case "interrupted": return t("已中断", "Interrupted", "已中斷")
        case "planning": return t("预检中", "Preflighting", "預檢中")
        case "ready": return t("预检完成", "Preflight ready", "預檢完成")
        case "pending": return t("等待处理", "Pending", "等待處理")
        case "partially_succeeded": return t("部分成功", "Partially succeeded", "部分成功")
        case "skipped": return t("已跳过", "Skipped", "已略過")
        default: return s
        }
    }
}
