pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
Item {
    id: root
    required property var controller
    property string section: "general"
    signal sectionRequested(string destination)
    property var draft: ({})
    property string savedSnapshot: ""
    readonly property bool dirty: JSON.stringify(draft) !== savedSnapshot
    function reset(): void {
        draft = {language: controller.language, density: controller.preferences.density || "standard", restoreLayout: controller.preferences.restoreLayout !== false, defaultFilesVisible: controller.preferences.defaultFilesVisible !== false, inspectorMode: controller.preferences.inspectorMode || "auto"}
        savedSnapshot = JSON.stringify(draft)
    }
    function change(key, value): void { const next = Object.assign({}, draft); next[key] = value; draft = next }
    function save(): void {
        for (const key of ["density", "restoreLayout", "defaultFilesVisible", "inspectorMode"]) controller.preference(key, draft[key])
        controller.language = draft.language
        savedSnapshot = JSON.stringify(draft)
    }
    Component.onCompleted: reset()
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() { if (root.visible) Qt.callLater(() => { const window = root.Window.window; if (window) Theme.revealIn(scroll, window.activeFocusItem) }) }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 32; spacing: 20
        ScrollView {
            id: scroll; objectName: "generalSettingsScroll"
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: Math.min(900, scroll.availableWidth); x: (scroll.availableWidth - width) / 2; spacing: 24
                SettingsGroup {
                    visible: root.section === "general"; title: Theme.t("外观", "Appearance", "外觀")
                    SettingsRow {
                        label: Theme.t("界面语言", "Interface language", "介面語言")
                        Choice { form: true; objectName: "settingsLanguage"; Layout.preferredWidth: 220; model: ["简体中文", "English", "繁體中文"]; currentIndex: ["zh-CN","en","zh-TW"].indexOf(root.draft.language); onActivated: root.change("language", ["zh-CN","en","zh-TW"][currentIndex]); Accessible.name: Theme.t("界面语言", "Interface language", "介面語言") }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
                    SettingsRow {
                        label: Theme.t("界面密度", "Interface density", "介面密度")
                        RowLayout {
                            spacing: 0
                            Tool { form: true; objectName: "settingsDensityStandard"; Layout.preferredWidth: 105; text: Theme.t("标准", "Standard", "標準"); checked: root.draft.density === "standard"; onClicked: root.change("density", "standard") }
                            Tool { form: true; objectName: "settingsDensityCompact"; Layout.preferredWidth: 105; text: Theme.t("紧凑", "Compact", "緊湊"); checked: root.draft.density === "compact"; onClicked: root.change("density", "compact") }
                        }
                    }
                }
                SettingsGroup {
                    visible: root.section === "general" || root.section === "preview"; title: Theme.t("启动与布局", "Startup and layout", "啟動與配置")
                    SettingsRow {
                        label: Theme.t("恢复上次布局", "Restore previous layout", "還原上次配置")
                        description: Theme.t("下次启动时保留文件面板宽度与展开状态", "Retain file pane width and visibility on next startup", "下次啟動時保留檔案面板寬度與展開狀態")
                        Toggle { objectName: "settingsRestoreLayout"; checked: root.draft.restoreLayout === true; onToggled: root.change("restoreLayout", checked); Accessible.name: parent.label }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
                    SettingsRow {
                        label: Theme.t("文件面板", "Files pane", "檔案面板")
                        Choice { form: true; objectName: "settingsFiles"; Layout.preferredWidth: 220; model: [Theme.t("默认展开", "Expanded by default", "預設展開"), Theme.t("默认收起", "Collapsed by default", "預設收起")]; currentIndex: root.draft.defaultFilesVisible ? 0 : 1; onActivated: root.change("defaultFilesVisible", currentIndex === 0); Accessible.name: parent.label }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
                    SettingsRow {
                        label: Theme.t("检查器", "Inspector", "檢查器")
                        Choice { form: true; objectName: "settingsInspector"; Layout.preferredWidth: 220; model: [Theme.t("按需显示", "Show with an asset", "按需顯示"), Theme.t("始终显示", "Always show", "一律顯示"), Theme.t("默认收起", "Collapsed", "預設收起")]; currentIndex: ["auto", "always", "hidden"].indexOf(root.draft.inspectorMode); onActivated: root.change("inspectorMode", ["auto","always","hidden"][currentIndex]); Accessible.name: parent.label }
                    }
                }
                SettingsGroup {
                    visible: root.section === "general" || root.section === "cache"; title: Theme.t("缓存", "Cache", "快取")
                    SettingsRow {
                        label: Theme.t("缓存位置", "Cache location", "快取位置")
                        description: Theme.t("当前使用内存缓存，不写入磁盘目录", "Uses memory cache; no disk cache directory", "目前使用記憶體快取，不寫入磁碟目錄")
                        Field { form: true; Layout.preferredWidth: 260; readOnly: true; text: Theme.t("进程内存", "Process memory", "程序記憶體"); Accessible.name: parent.label }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
                    SettingsRow {
                        visible: root.section !== "cache"; label: Theme.t("缓存管理", "Cache management", "快取管理")
                        Tool { form: true; quiet: true; iconName: "expand"; text: Theme.t("查看缓存信息", "View cache details", "檢視快取資訊"); onClicked: root.sectionRequested("cache") }
                    }
                    SettingsRow { visible: root.section === "cache"; label: Theme.t("缩略图缓存", "Thumbnail cache", "縮圖快取"); description: Theme.t("按需加载；最多占用 32 MiB，退出时释放。", "Loaded on demand; capped at 32 MiB and released on exit.", "按需載入；最多使用 32 MiB，結束時釋放。") }
                }
                SettingsGroup {
                    visible: root.section === "about"; title: Theme.t("关于 FormaView", "About FormaView", "關於 FormaView")
                    SettingsRow { label: "FormaView · Qt 6.12"; description: Theme.t("只读资产预览、图片检查与独立转换导出。预览调整不修改原文件。", "Read-only asset preview, image inspection and separate conversion exports. Preview adjustments leave source files unchanged.", "唯讀資產預覽、影像檢查與獨立轉換匯出。預覽調整不修改原始檔案。") }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: false; Layout.preferredWidth: Math.min(900, root.width - 64); Layout.maximumWidth: 900; Layout.alignment: Qt.AlignHCenter; spacing: 12
            Label { Layout.fillWidth: true; color: Theme.muted; font.pixelSize: 12; wrapMode: Text.WordWrap; text: Theme.t("修改后保存生效；切换设置页保留草稿。", "Save to apply changes; drafts are retained between settings pages.", "修改後儲存生效；切換設定頁保留草稿。") }
            Tool { form: true; objectName: "discardGeneralSettings"; text: Theme.t("放弃更改", "Discard changes", "放棄變更"); enabled: root.dirty; onClicked: root.reset() }
            Tool { form: true; objectName: "saveGeneralSettings"; primary: true; text: Theme.t("保存设置", "Save settings", "儲存設定"); enabled: root.dirty; onClicked: root.save() }
        }
    }
}
