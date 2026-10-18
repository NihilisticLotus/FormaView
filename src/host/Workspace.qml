import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
Item {
    id: root
    objectName: "workspace"
    required property var controller
    readonly property real navigationWidth: narrow ? 56 : 200
    readonly property string currentPage: settingsVisible ? "settings" : conversionVisible ? "conversion" : tasksVisible ? "tasks" : "assets"
    property bool focusMode: false
    property bool tasksVisible: false
    property bool conversionVisible: false
    property bool settingsVisible: false
    property bool filesVisible: controller.preferences.filesVisible !== false
    readonly property bool inspectorVisible: !focusMode && (controller.preferences.inspectorMode || "auto") !== "hidden" && (controller.preferences.inspectorMode === "always" || !!controller.asset.path)
    function requestWindowClose(): bool {
        if (!settingsVisible || !settingsDialog.dirty) return true
        settingsDialog.leave(() => root.Window.window.close())
        return false
    }
    function openConversion(page) {
        if (!controller.imageJobs) return
        if (settingsVisible) { settingsDialog.leave(() => root.openConversion(page)); return }
        tasksVisible = false; settingsVisible = false
        if (drawer.visible) drawer.close()
        conversionVisible = true
        if (!conversionLoader.item) conversionLoader.setSource("ConversionWorkspace.qml", {controller: root.controller})
        const workspace = conversionLoader.item as ConversionWorkspace
        if (workspace) workspace.open(page)
    }
    readonly property bool narrow: width <= 1300
    function hasFocusWithin(pane) { if (!pane) return false; for (let item = root.Window.window.activeFocusItem; item; item = item.parent) if (item === pane) return true; return false }
    function returnFileFocus() { Qt.callLater(() => { if (root.tasksVisible) return; if (fileRail.visible) fileRail.forceActiveFocus(); else if (files.visible) files.addressFocus(); else preview.focusLayoutControl() }) }
    function openAddress() {
        if (settingsVisible) { settingsDialog.leave(() => root.openAddress()); return }
        settingsVisible = false; conversionVisible = false; tasksVisible = false; focusMode = false; filesVisible = true
        controller.preference("filesVisible", true)
        if (narrow) { drawer.open(); if (drawer.opened) drawerFiles.addressFocus() }
        else Qt.callLater(() => { if (files.visible) files.addressFocus() })
    }
    function toggleFiles() {
        if (narrow) { if (drawer.visible) drawer.close(); else openAddress(); return }
        if (focusMode || !filesVisible) { openAddress(); return }
        files.remember(); filesVisible = false; controller.preference("filesVisible", false); returnFileFocus()
    }
    onFocusModeChanged: { files.remember(); if (drawer.visible) drawer.close(); Qt.callLater(() => { if (!root.tasksVisible) preview.focusLayoutControl() }) }
    onNarrowChanged: { if (narrow && hasFocusWithin(files)) returnFileFocus(); else if (!narrow && drawer.visible) drawer.close() }
    function editing(): bool { const item = root.Window.window.activeFocusItem; return !!item && (item.hasOwnProperty("inputMethodComposing") || item.hasOwnProperty("selectByMouse")) }
    Shortcut { sequence: "Ctrl+O"; onActivated: fileDialog.open() }
    Shortcut { sequence: "Ctrl+L"; onActivated: root.openAddress() }
    Shortcut { sequence: "Alt+Left"; enabled: !root.editing(); onActivated: root.controller.history(-1) }
    Shortcut { sequence: "Alt+Right"; enabled: !root.editing(); onActivated: root.controller.history(1) }
    Shortcut { sequence: "Alt+Up"; enabled: !root.editing(); onActivated: root.controller.parentDirectory() }
    function navigate(destination): void {
        if (settingsVisible && ["conversion", "tasks"].includes(destination)) { settingsDialog.leave(() => root.navigate(destination)); return }
        if (settingsVisible && destination === "assets") { settingsDialog.close(); return }
        if (["general", "preview", "cache", "permissions", "about"].includes(destination)) { settingsDialog.section = destination; return }
        if (destination === "settings") { settingsDialog.open("general"); return }
        if (destination === "conversion") { openConversion(0); return }
        settingsVisible = false; conversionVisible = false; tasksVisible = destination === "tasks"
    }
    NavigationRail {
        id: navigation; objectName: "navigationRail"
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.bottomMargin: Theme.statusHeight
        width: root.navigationWidth; page: root.currentPage; collapsed: root.narrow; section: settingsDialog.section; conversionAvailable: !!root.controller.imageJobs
        onNavigate: destination => root.navigate(destination)
    }
    ColumnLayout {
        anchors.fill: parent; anchors.leftMargin: root.navigationWidth; spacing: 0
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: Theme.topBarHeight; color: Theme.base
            RowLayout {
                objectName: "topBar"; anchors.fill: parent; anchors.leftMargin: 24; anchors.rightMargin: 24; spacing: 12
                AppIcon { name: root.settingsVisible ? "general" : root.conversionVisible ? "conversion" : root.tasksVisible ? "tasks" : "assets"; size: 20 }
                Label { font.pixelSize: 18; font.bold: true; color: Theme.text; text: root.settingsVisible ? Theme.t("设置", "Settings", "設定") : root.conversionVisible ? Theme.t("图片批量转换", "Batch image conversion", "影像批次轉換") : root.tasksVisible ? Theme.t("任务中心", "Task center", "工作中心") : Theme.t("资产工作区", "Asset workspace", "資產工作區") }
                Label { Layout.fillWidth: true; visible: root.currentPage === "assets"; text: root.controller.directory.replace(/\\/g, "/").split("/").pop(); elide: Text.ElideMiddle; color: Theme.muted }
                Item { visible: root.currentPage !== "assets"; Layout.fillWidth: true }
                Tool { visible: root.currentPage === "assets"; iconName: "assets"; primary: true; text: Theme.t("打开目录", "Open folder", "開啟資料夾"); onClicked: folderDialog.open() }
                Tool {
                    objectName: "applicationMenuButton"; iconName: "more"; quiet: true; hint: Theme.t("应用菜单", "Application menu", "應用程式選單")
                    onClicked: mainMenu.open()
                    Popup {
                        id: mainMenu; y: parent.height; x: Math.min(0, parent.width - width); padding: 8
                        background: Rectangle { color: Theme.raised; radius: Theme.groupRadius; border.color: Theme.line }
                        contentItem: AppMenu {
                            controller: root.controller
                            onOpenFile: { mainMenu.close(); fileDialog.open() }
                            onOpenFolder: { mainMenu.close(); folderDialog.open() }
                            onToggleFiles: { mainMenu.close(); root.toggleFiles() }
                            onToggleFocus: { mainMenu.close(); root.focusMode = !root.focusMode }
                            onTasks: { mainMenu.close(); root.navigate("tasks") }
                            onSettings: { mainMenu.close(); settingsDialog.open("permissions") }
                            onUnavailable: { mainMenu.close(); about.open() }
                            onConvert: { mainMenu.close(); root.openConversion(0) }
                        }
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            RowLayout {
                anchors.fill: parent; anchors.margins: 8; spacing: 8; visible: !root.tasksVisible && !root.conversionVisible && !root.settingsVisible
                Rectangle {
                    Layout.fillHeight: true; Layout.preferredWidth: 40; color: Theme.panel
                    visible: root.focusMode || root.narrow || !root.filesVisible
                    Tool { id: fileRail; objectName: "fileRail"; width: 38; iconName: "files-open"; hint: Theme.t("展开文件区", "Show files", "展開檔案區"); Accessible.name: hint; onClicked: root.openAddress() }
                }
                FilesPane {
                    id: files
                    Layout.fillHeight: true; Layout.preferredWidth: Math.max(220,Math.min(400, root.controller.preferences.fileWidth || Theme.filesWidth))
                    visible: !root.narrow && !root.focusMode && root.filesVisible
                    controller: root.controller; onCollapse: root.toggleFiles()
                Rectangle {
                    anchors.right: parent.right; height: parent.height; width: 4; z: 2; color: mouse.containsMouse ? Theme.accent : Theme.base
                    MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.SplitHCursor; property real start: 0; property real old: 0
                        onPressed: mouse => { start = mapToItem(root,mouse.x,0).x; old = files.width }
                        onPositionChanged: mouse => { if(pressed) root.controller.preference("fileWidth",Math.max(220, Math.min(400,old+mapToItem(root,mouse.x,0).x-start))) }
                    }
                }
                }
                PreviewPane {
                    id: preview; Layout.fillWidth: true; Layout.fillHeight: true; controller: root.controller; focused: root.focusMode
                    onOpenFile: fileDialog.open(); onOpenFolder: folderDialog.open(); onLocate: locateDialog.open()
                    onToggleFocus: { root.conversionVisible = false; root.focusMode = !root.focusMode }
                    onUnavailable: about.open()
                }
                Inspector { Layout.fillHeight: true; Layout.preferredWidth: root.narrow ? Theme.narrowInspectorWidth : Theme.inspectorWidth; visible: root.inspectorVisible; controller: root.controller }
                Rectangle { visible: root.focusMode; Layout.fillHeight: true; Layout.preferredWidth: 40; color: Theme.panel; Tool { objectName: "inspectorRail"; width: 38; iconName: "inspector-open"; hint: Theme.t("展开检查区", "Show inspector", "展開檢查區"); Accessible.name: hint; onClicked: root.focusMode = false } }
            }
            TasksPane { anchors.fill: parent; visible: root.tasksVisible && !root.conversionVisible && !root.settingsVisible; controller: root.controller; onBack: root.tasksVisible = false; onRetryImageJob: id => { root.controller.imageJobs.retry(id); root.openConversion(1) } }
            SettingsDialog {
                id: settingsDialog; anchors.fill: parent; controller: root.controller; visible: root.settingsVisible
                onOpened: { root.settingsVisible = true; root.tasksVisible = false; root.conversionVisible = false }
                onClosed: { root.settingsVisible = false; preview.focusLayoutControl() }
            }
            Loader { id: conversionLoader; anchors.fill: parent; visible: root.conversionVisible && !root.settingsVisible }
        }
        Item { Layout.fillWidth: true; Layout.preferredHeight: Theme.statusHeight }
    }
    Rectangle {
        objectName: "statusBar"; anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: Theme.statusHeight; color: Theme.panel
        Rectangle { width: parent.width; height: 1; color: Theme.line }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 24; anchors.rightMargin: 24; spacing: 12
            AppIcon { name: root.controller.busy ? "loading" : "success"; size: 16; variant: "muted" }
            Label { color: Theme.muted; font.pixelSize: 12; text: root.controller.busy ? Theme.t("正在扫描目录…", "Scanning folder…", "正在掃描資料夾…") : Theme.t("就绪", "Ready", "就緒") }
            Item { Layout.fillWidth: true }
            Label { color: Theme.muted; font.pixelSize: 12; text: root.controller.activeTasks ? Theme.t("活动任务 · ", "Active tasks · ", "活動工作 · ") + root.controller.activeTasks : Theme.t("暂无活动任务", "No active tasks", "暫無活動工作") }
        }
    }
    Drawer {
        id: drawer
        objectName: "fileDrawer"
        width: 288; height: root.height; modal: true; focus: true
        closePolicy: Popup.CloseOnPressOutside
        FilesPane { id: drawerFiles; anchors.fill: parent; controller: root.controller; onCollapse: drawer.close() }
        onAboutToHide: drawerFiles.remember()
        onOpened: drawerFiles.addressFocus()
        onClosed: root.returnFileFocus()
    }
    FileDialog { id: fileDialog; title: Theme.t("打开文件", "Open file", "開啟檔案"); onAccepted: root.controller.openPath(selectedFile.toString()) }
    FolderDialog { id: folderDialog; title: Theme.t("打开目录", "Open folder", "開啟資料夾"); onAccepted: root.controller.navigate(selectedFolder.toString()) }
    FileDialog { id: locateDialog; title: Theme.t("定位替代文件 · 读取检查后应用", "Locate replacement · Check before applying", "定位替代檔案 · 讀取檢查後套用"); onAccepted: root.controller.openPath(selectedFile.toString()) }
    Connections { target: conversionLoader.item; function onClosed() { root.conversionVisible = false; Qt.callLater(() => navigation.focusDestination("conversion")) } }
    Dialog {
        id: about; anchors.centerIn: Overlay.overlay; width: 480; modal: true; focus: true
        footer: DialogButtonBox {
            Button { text: Theme.t("关闭", "Close", "關閉"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            onRejected: about.reject()
        }
        title: "FormaView"
        Label { width: parent.width; wrapMode: Text.WordWrap; color: Theme.text; text: (root.controller.imageJobs ? Theme.t("支持图片通道、像素读数、子资源与批量转换。MCP 默认关闭，需在设置中应用范围并启用。模型与动画预览仍待接入。", "Image channels, pixel values, subresources and batch conversion are available. MCP is off by default; apply a scope and enable it in Settings. Model and animation preview are pending.", "支援影像通道、像素讀數、子資源與批次轉換。MCP 預設關閉，須在設定套用範圍並啟用。模型與動畫預覽仍待接入。") : Theme.t("此构建提供文件浏览、缩略图、读取检查与任务中心。未编入图片服务。", "This build provides file browsing, thumbnails, access checks and task status. The image service is not included.", "此組建提供檔案瀏覽、縮圖、讀取檢查與工作中心。未編入影像服務。")) + Theme.t("\n\nCtrl+O 打开文件 · Ctrl+L 编辑位置\nAlt+方向键 导航 · 方向键连续浏览\nCtrl / Shift 多选 · Esc 关闭弹层", "\n\nCtrl+O Open · Ctrl+L Location\nAlt+Arrows Navigate · Arrows Browse\nCtrl / Shift Multi-select · Esc Close popup", "\n\nCtrl+O 開啟檔案 · Ctrl+L 編輯位置\nAlt+方向鍵 導覽 · 方向鍵連續瀏覽\nCtrl / Shift 多選 · Esc 關閉彈層") }
    }
    DropArea { anchors.fill: parent; enabled: !root.conversionVisible && !root.settingsVisible; onDropped: drop => { if(drop.hasUrls) { root.controller.openUrls(drop.urls); drop.acceptProposedAction() } } }
}
