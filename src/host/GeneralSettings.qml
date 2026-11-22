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
        controller.refreshExplorerPreferences()
        draft = {reuseImageWindow: controller.preferences.reuseImageWindow !== false, reuseModelWindow: controller.preferences.reuseModelWindow !== false, reuseVideoWindow: controller.preferences.reuseVideoWindow !== false, language: controller.language, appearance: controller.preferences.appearance || "light", reduceMotion: controller.preferences.reduceMotion === true, density: controller.preferences.density || "standard", videoForward: VideoPreferences.forward, videoBackward: VideoPreferences.backward, defaultAxes: controller.preferences.defaultAxes || "source", defaultUnit: controller.preferences.defaultUnit || "source", defaultImageTarget: controller.preferences.defaultImageTarget || "png", restoreLayout: controller.preferences.restoreLayout !== false, defaultFilesVisible: controller.preferences.defaultFilesVisible !== false, inspectorMode: controller.preferences.inspectorMode || "auto"}
        savedSnapshot = JSON.stringify(draft)
    }
    function change(key, value): void { const next = Object.assign({}, draft); next[key] = value; draft = next }
    function save(): void {
        const saved = JSON.parse(savedSnapshot)
        for (const key of ["reuseImageWindow", "reuseModelWindow", "reuseVideoWindow", "appearance", "defaultImageTarget", "defaultAxes", "defaultUnit", "reduceMotion", "density", "restoreLayout", "defaultFilesVisible", "inspectorMode"])
            if (saved[key] !== draft[key]) controller.preference(key, draft[key])
        if (draft.videoForward !== saved.videoForward) VideoPreferences.forward = draft.videoForward
        if (draft.videoBackward !== saved.videoBackward) VideoPreferences.backward = draft.videoBackward
        VideoPreferences.sync()
        controller.language = draft.language
        savedSnapshot = JSON.stringify(draft)
    }
    Component.onCompleted: reset()
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() { if (root.visible) Qt.callLater(() => { const window = root.Window.window; if (window) Theme.revealIn(scroll, window.activeFocusItem) }) }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 20; spacing: 16
        ScrollView {
            id: scroll; objectName: "generalSettingsScroll"
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: scroll.availableWidth; spacing: 18
                Label {
                    Layout.fillWidth: true; color: Theme.text; font.pixelSize: 22; font.bold: true
                    text: ({"general": Theme.t("通用与外观","General & appearance","一般與外觀"), "preview": Theme.t("预览与窗口","Preview & windows","預覽與視窗"), "video": Theme.t("视频播放","Video playback","影片播放"), "export": Theme.t("导出默认值","Export defaults","匯出預設值"), "keys":Theme.t("快捷键","Keyboard shortcuts","快速鍵"),"cache":Theme.t("缓存","Cache","快取"),"about":Theme.t("关于","About","關於")})[root.section] || ""
                }
                SettingsGroup {
                    visible: root.section === "general"; title: Theme.t("外观", "Appearance", "外觀")
                    RowLayout {
                        Layout.fillWidth: true; Layout.topMargin: 12; Layout.bottomMargin: 12; spacing: 12
                        Repeater {
                            model: ["light","dark"]
                            delegate: Button {
                                id: option
                                required property string modelData
                                readonly property bool silver: modelData === "light"
                                objectName: "appearance-" + modelData
                                text: silver ? Theme.t("浅银玻璃","Silver glass","淺銀玻璃") : Theme.t("深色玻璃","Dark glass","深色玻璃")
                                checked: root.draft.appearance === modelData
                                Accessible.name: text
                                Layout.preferredWidth: 148; Layout.maximumWidth: 160; Layout.fillWidth: true; implicitHeight: 100
                                padding: 6
                                onClicked: root.change("appearance",modelData)
                                background: Rectangle {
                                    radius: 10; color: option.hovered ? Theme.glass : "transparent"
                                    border.color: option.checked || option.visualFocus ? Theme.accent : "transparent"; border.width: 2
                                }
                                contentItem: ColumnLayout {
                                    spacing: 5
                                    Rectangle {
                                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 6; clip: true
                                        color: option.silver ? "#eaf1f5" : "#252a34"
                                        Rectangle { x: 1; y: 1; width: parent.width * .25; height: parent.height-2; radius: 4; color: option.silver ? "#dae7f2" : "#343d49" }
                                        Rectangle { x: parent.width*.34; y: parent.height*.19; width: parent.width*.54; height: 4; radius: 2; color: option.silver ? "#ccd7df" : "#5a6571" }
                                        Rectangle { x: parent.width*.34; y: parent.height*.39; width: parent.width*.54; height: parent.height*.43; radius: 3; color: option.silver ? "#d5e4ec" : "#191f28" }
                                    }
                                    RowLayout {
                                        Label { Layout.fillWidth: true; text: option.text; color: Theme.text; font.pixelSize: 13 }
                                        AppIcon { visible: option.checked; name: "confirm"; size: 15 }
                                    }
                                }
                            }
                        }
                        Item { Layout.fillWidth: true; Layout.preferredWidth: 100 }
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
                    SettingsRow {
                        label: Theme.t("界面语言", "Interface language", "介面語言")
                        Choice { form: true; objectName: "settingsLanguage"; Layout.preferredWidth: 220; model: ["简体中文", "English", "繁體中文"]; currentIndex: ["zh-CN","en","zh-TW"].indexOf(root.draft.language); onActivated: root.change("language", ["zh-CN","en","zh-TW"][currentIndex]); Accessible.name: Theme.t("界面语言", "Interface language", "介面語言") }
                    }
                    SettingsRow {
                        label: Theme.t("减少动态效果","Reduce motion","減少動態效果")
                        Toggle { objectName: "settingsReduceMotion"; checked: root.draft.reduceMotion === true; onToggled: root.change("reduceMotion", checked); Accessible.name: parent.label }
                    }
                }
                SettingsGroup {
                    visible: root.section === "general" || root.section === "preview"; title: Theme.t("启动与布局", "Startup and layout", "啟動與配置")
                    SettingsRow {
                        label: Theme.t("恢复上次布局", "Restore previous layout", "還原上次配置")
                        description: Theme.t("下次启动时保留文件面板宽度与展开状态", "Retain file pane width and visibility on next startup", "下次啟動時保留檔案面板寬度與展開狀態")
                        Toggle { objectName: "settingsRestoreLayout"; checked: root.draft.restoreLayout === true; onToggled: root.change("restoreLayout", checked); Accessible.name: parent.label }
                    }
                }
                SettingsGroup {
                    visible: root.section === "preview"; title: Theme.t("窗口","Windows","視窗")
                    SettingsRow {
                        label: Theme.t("双击图片时复用窗口", "Reuse window for images", "雙擊圖片時重用視窗")
                        Toggle { checked: root.draft.reuseImageWindow === true; onToggled: root.change("reuseImageWindow", checked); Accessible.name: parent.label }
                    }
                    SettingsRow {
                        label: Theme.t("双击模型时复用窗口", "Reuse window for models", "雙擊模型時重用視窗")
                        Toggle { checked: root.draft.reuseModelWindow === true; onToggled: root.change("reuseModelWindow", checked); Accessible.name: parent.label }
                    }
                    SettingsRow {
                        label: Theme.t("双击视频时复用窗口", "Reuse window for videos", "雙擊影片時重用視窗")
                        Toggle { objectName: "settingsReuseVideoWindow"; checked: root.draft.reuseVideoWindow === true; onToggled: root.change("reuseVideoWindow", checked); Accessible.name: parent.label }
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
                    visible: root.section === "cache"; title: Theme.t("缓存", "Cache", "快取")
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
                    visible: root.section === "video"; title: Theme.t("快进与回退","Seek intervals","快進與倒退")
                    SettingsRow {
                        label: Theme.t("快进（秒）","Forward (seconds)","快進（秒）")
                        NumberField { objectName: "settingsVideoForward"; form: true; from: 1; to: 600; value: root.draft.videoForward || 5; onValueModified: root.change("videoForward",value); Accessible.name: parent.label }
                    }
                    SettingsRow {
                        label: Theme.t("回退（秒）","Backward (seconds)","倒退（秒）")
                        NumberField { form: true; from: 1; to: 600; value: root.draft.videoBackward || 5; onValueModified: root.change("videoBackward",value); Accessible.name: parent.label }
                    }
                    SettingsRow {
                        label: Theme.t("双击视频时复用窗口","Reuse window for videos","雙擊影片時重用視窗")
                        Toggle { checked: root.draft.reuseVideoWindow === true; onToggled: root.change("reuseVideoWindow", checked); Accessible.name: parent.label }
                    }
                }
                SettingsGroup {
                    visible: root.section === "export"; title: Theme.t("图片","Images","影像")
                    SettingsRow {
                        label: Theme.t("新建转换的目标格式","Default image format","新增轉換的目標格式")
                        Choice { id: defaultFormat; objectName: "defaultImageTarget"; form: true; Layout.preferredWidth: 165
                            readonly property var formats: root.controller.imageJobs?.capabilities.write_targets || []
                            labels: formats.map(f => f.toUpperCase())
                            currentIndex: formats.indexOf(root.draft.defaultImageTarget)
                            enabled: formats.length > 0
                            onActivated: root.change("defaultImageTarget", formats[currentIndex])
                        }
                    }
                }
                SettingsGroup {
                    visible: root.section === "export" && !!root.controller.imageJobs?.capabilities.model_export
                    title: Theme.t("模型与动画","Models and animation","模型與動畫")
                    SettingsRow {
                        label: Theme.t("坐标预设","Coordinate preset","座標預設")
                        Choice { form: true; Layout.preferredWidth: 165
                            readonly property var values: ["source","maya","houdini","unreal","3ds-max","cinema4d","blender","unity"]
                            labels: [Theme.t("保留源坐标","Source axes","保留來源座標"),"Maya","Houdini","Unreal","3ds Max","Cinema 4D","Blender","Unity"]
                            currentIndex: values.indexOf(root.draft.defaultAxes)
                            onActivated: root.change("defaultAxes", values[currentIndex]); Accessible.name: parent.label
                        }
                    }
                    SettingsRow {
                        label: Theme.t("输出单位","Output units","輸出單位")
                        Choice { form: true; Layout.preferredWidth: 165
                            readonly property var values: ["source","cm","m","mm","in"]
                            labels: [Theme.t("保留源单位","Source units","保留來源單位"),"cm","m","mm","in"]
                            currentIndex: values.indexOf(root.draft.defaultUnit)
                            onActivated: root.change("defaultUnit", values[currentIndex]); Accessible.name: parent.label
                        }
                    }
                }
                SettingsGroup {
                    visible: root.section === "keys"; title: Theme.t("工作区与播放","Workspace and playback","工作區與播放")
                    Repeater {
                        model: [[Theme.t("打开文件","Open file","開啟檔案"),"Ctrl + O"],[Theme.t("目录地址","Folder address","目錄位址"),"Ctrl + L"],[Theme.t("播放 / 暂停","Play / pause","播放 / 暫停"),"Space"],[Theme.t("上一帧 / 下一帧","Previous / next frame","上一幀 / 下一幀"),",  /  ."],[Theme.t("调整音量","Volume","調整音量"),"↑  /  ↓"]]
                        delegate: SettingsRow {
                            required property var modelData
                            label: modelData[0]
                            Label { text: modelData[1]; color: Theme.text }
                        }
                    }
                }
                SettingsGroup {
                    visible: root.section === "about"; title: Theme.t("关于 FormaView", "About FormaView", "關於 FormaView")
                    SettingsRow { label: "FormaView " + Qt.application.version + " · Qt 6.12"; description: Theme.t("只读资产预览、图片检查与独立转换导出。预览调整不修改原文件。", "Read-only asset preview, image inspection and separate conversion exports. Preview adjustments leave source files unchanged.", "唯讀資產預覽、影像檢查與獨立轉換匯出。預覽調整不修改原始檔案。") }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: false; Layout.preferredWidth: root.width - 40; Layout.alignment: Qt.AlignHCenter; spacing: 12
            Label { Layout.fillWidth: true; color: Theme.muted; font.pixelSize: 12; wrapMode: Text.WordWrap; text: Theme.t("修改后保存生效；切换设置页保留草稿。", "Save to apply changes; drafts are retained between settings pages.", "修改後儲存生效；切換設定頁保留草稿。") }
            Tool { form: true; objectName: "discardGeneralSettings"; text: Theme.t("放弃更改", "Discard changes", "放棄變更"); enabled: root.dirty; onClicked: root.reset() }
            Tool { form: true; objectName: "saveGeneralSettings"; primary: true; text: Theme.t("保存设置", "Save settings", "儲存設定"); enabled: root.dirty; onClicked: root.save() }
        }
    }
}
