pragma Singleton
import QtQuick
QtObject {
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
    readonly property int statusHeight: 44
    readonly property int panelGap: 8
    readonly property int filesWidth: 236
    readonly property int inspectorWidth: 312
    readonly property int narrowInspectorWidth: 300
    readonly property int railWidth: 40
    readonly property int radius: 6
    readonly property int groupRadius: 10
    readonly property int dialogRadius: 14
    readonly property color hover: "#38383e"
    property string language: "zh-CN"
    readonly property color base: "#1c1c1e"
    readonly property color sidebar: "#161618"
    readonly property color panel: "#26262a"
    readonly property color raised: "#2e2e33"
    readonly property color field: "#26262a"
    readonly property color line: "#3e3e45"
    readonly property color text: "#e4e4e7"
    readonly property color muted: "#ababb3"
    readonly property color tertiary: "#9a9aa2"
    readonly property color accent: "#d8783a"
    readonly property color action: "#c74c0a"
    readonly property color actionHover: "#b84308"
    readonly property color actionPressed: "#a33b07"
    readonly property color selection: "#313136"
    readonly property color error: "#f87171"
    readonly property color success: "#4ade80"
    readonly property color warning: "#fbbf24"
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
