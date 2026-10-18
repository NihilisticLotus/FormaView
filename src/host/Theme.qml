pragma Singleton
import QtQuick
QtObject {
    property string language: "zh-CN"
    readonly property color base: "#222326"
    readonly property color panel: "#2b2d30"
    readonly property color raised: "#34363a"
    readonly property color field: "#242629"
    readonly property color line: "#45474b"
    readonly property color text: "#e4e5e7"
    readonly property color muted: "#adb0b6"
    readonly property color accent: "#dfac62"
    readonly property color selection: "#494237"
    readonly property color error: "#f1a49a"
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
