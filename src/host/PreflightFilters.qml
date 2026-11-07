pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
    objectName: "preflightFilters"
    required property var items
    property var selectedStates: []
    property string selectedFormat: ""
    readonly property string keyword: search.text.trim().toLocaleLowerCase()
    readonly property var stateCounts: countStates()
    readonly property int warningCount: items.filter(i => hasWarnings(i)).length
    readonly property var states: Object.keys(stateCounts)
    readonly property var formats: {
        const values = []
        for (const entry of items) {
            const value = String(entry.request?.options?.target || "")
            if (value && !values.includes(value)) values.push(value)
        }
        if (selectedFormat && !values.includes(selectedFormat)) values.push(selectedFormat)
        return [""].concat(values.sort())
    }
    readonly property var filteredItems: items.filter(entry => {
        if (selectedStates.length && !selectedStates.includes(entry.status || "pending")) return false
        if (warnings.checked && !hasWarnings(entry)) return false
        const request = entry.request || {}, target = String(request.options?.target || "")
        if (kind.currentIndex === 1 && target === "fbx") return false
        if (kind.currentIndex === 2 && target !== "fbx") return false
        if (selectedFormat && target !== selectedFormat) return false
        const paths = [request.input, request.output, entry.preflight?.output,
            ...(entry.preflight?.clips || []).map(c => c.name)].join("\n").toLocaleLowerCase()
        return !keyword || paths.includes(keyword)
    })
    readonly property bool active: !!keyword || selectedStates.length > 0 || warnings.checked || kind.currentIndex > 0 || !!selectedFormat
    signal filterChanged()
    onSelectedStatesChanged: filterChanged()
    onSelectedFormatChanged: filterChanged()
    onKeywordChanged: filterChanged()
    function hasWarnings(entry): bool { return (entry.preflight?.losses || []).length > 0 || (entry.preflight?.warnings || []).length > 0 }
    function countStates(): var {
        const counts = {}
        for (const entry of items) {
            const state = entry.status || "pending"
            counts[state] = (counts[state] || 0) + 1
        }
        for (const state of selectedStates) if (!(state in counts)) counts[state] = 0
        return counts
    }
    function stateLabel(state): string {
        return state === "ready" ? Theme.t("预检成功", "Passed", "預檢成功") : Theme.state(state)
    }
    function reset(): void {
        search.clear(); selectedStates = []; selectedFormat = ""; warnings.checked = false; kind.currentIndex = 0
    }
    GridLayout {
        Layout.fillWidth: true; columns: root.width < 760 ? 2 : 4; columnSpacing: 10; rowSpacing: 8
        Field {
            id: search; objectName: "preflightSearch"; Layout.fillWidth: true; Layout.minimumWidth: 160
            placeholderText: Theme.t("搜索片段名、来源或输出路径", "Search clip, source or output path", "搜尋片段名、來源或輸出路徑")
            Accessible.name: placeholderText
        }
        Choice {
            id: kind; objectName: "preflightKind"; Layout.preferredWidth: 160
            labels: [Theme.t("全部转换类型", "All conversions", "全部轉換類型"), Theme.t("图片转换", "Images", "影像轉換"), Theme.t("模型与动画", "Models and animations", "模型與動畫")]
            onCurrentIndexChanged: root.filterChanged()
            Accessible.name: Theme.t("转换类型筛选", "Filter by conversion type", "轉換類型篩選")
        }
        Choice {
            objectName: "preflightFormat"; Layout.preferredWidth: 150
            labels: root.formats.map(f => f ? f.toUpperCase() : Theme.t("全部输出格式", "All output formats", "全部輸出格式"))
            currentIndex: root.formats.indexOf(root.selectedFormat)
            onActivated: root.selectedFormat = root.formats[currentIndex]
            Accessible.name: Theme.t("输出格式筛选", "Filter by output format", "輸出格式篩選")
        }
        Tool {
            objectName: "clearPreflightFilters"; quiet: true; enabled: root.active
            text: Theme.t("清空筛选", "Clear filters", "清空篩選"); onClicked: root.reset()
        }
    }
    Flow {
        Layout.fillWidth: true; spacing: 12
        Repeater {
            model: root.states
            OptionCheck {
                required property string modelData
                objectName: "preflightState-" + modelData
                text: root.stateLabel(modelData) + " · " + root.stateCounts[modelData]
                checked: root.selectedStates.includes(modelData)
                onToggled: root.selectedStates = checked ? root.selectedStates.concat([modelData]) : root.selectedStates.filter(s => s !== modelData)
            }
        }
        OptionCheck {
            id: warnings; objectName: "preflightWarnings"; visible: root.warningCount > 0 || checked
            text: Theme.t("有警告 / 损失", "Warnings / losses", "有警告 / 損失") + " · " + root.warningCount
            onCheckedChanged: root.filterChanged()
        }
    }
    Label {
        objectName: "preflightFilteredCount"; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap
        text: Theme.t("显示 %1 / %2 项 · 同类状态任选，其余条件同时满足。筛选仅改变显示，提交仍包含整个计划。", "Showing %1 / %2 items · Match any selected state and all other filters. Filters affect the view; submission includes the entire plan.", "顯示 %1 / %2 項 · 同類狀態任選，其餘條件同時滿足。篩選只改變顯示，提交仍包含整個計畫。").arg(root.filteredItems.length).arg(root.items.length)
    }
}
