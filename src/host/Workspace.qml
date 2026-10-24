import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
Item {
    id: root
    objectName: "workspace"
    required property var controller
    readonly property real navigationWidth: focusMode && currentPage === "assets" ? 0 : narrow ? 56 : 200
    readonly property string currentPage: settingsVisible ? "settings" : conversionVisible ? "conversion" : tasksVisible ? "tasks" : "assets"
    readonly property bool settingsDirty: settingsVisible && settingsDialog.dirty
    property bool focusMode: false
    function setFocusMode(enabled): void {
        if (enabled && !focusMode) files.remember()
        focusMode = enabled
    }
    Keys.onEscapePressed: event => {
        if (root.focusMode && root.currentPage === "assets") {
            root.focusMode = false
            event.accepted = true
        } else event.accepted = false
    }
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
        else Qt.callLater(() => folderNavigation.addressFocus())
    }
    function toggleFiles() {
        if (narrow) { if (drawer.visible) drawer.close(); else openAddress(); return }
        if (focusMode || !filesVisible) { openAddress(); return }
        files.remember(); filesVisible = false; controller.preference("filesVisible", false); returnFileFocus()
    }
    onFocusModeChanged: { if (drawer.visible) drawer.close(); Qt.callLater(() => { if (!root.tasksVisible) preview.focusLayoutControl() }) }
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
        width: root.navigationWidth; visible: width > 0; page: root.currentPage; collapsed: root.narrow; section: settingsDialog.section; conversionAvailable: !!root.controller.imageJobs
        onNavigate: destination => root.navigate(destination)
    }
    ColumnLayout {
        anchors.fill: parent; anchors.leftMargin: root.navigationWidth; spacing: 0
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: Theme.topBarHeight; color: Theme.base
            MouseArea {
                id: titleDrag
                objectName: "windowTitleDrag"
                anchors.fill: parent; anchors.rightMargin: 160
                onPressed: root.Window.window.startSystemMove()
                onDoubleClicked: {
                    const window = root.Window.window
                    if (window.visibility === Window.Maximized) window.showNormal()
                    else window.showMaximized()
                }
            }
            RowLayout {
                objectName: "topBar"; anchors.fill: parent; anchors.leftMargin: 24; anchors.rightMargin: 160; spacing: 12
                AppIcon { name: root.settingsVisible ? "general" : root.conversionVisible ? "conversion" : root.tasksVisible ? "tasks" : "assets"; size: 20 }
                Label { font.pixelSize: 18; font.bold: true; color: Theme.text; text: root.settingsVisible ? Theme.t("设置", "Settings", "設定") : root.conversionVisible ? Theme.t("资产转换与导出", "Asset conversion and export", "資產轉換與匯出") : root.tasksVisible ? Theme.t("任务中心", "Task center", "工作中心") : Theme.t("资产工作区", "Asset workspace", "資產工作區") }
                Label { Layout.fillWidth: true; visible: root.currentPage === "assets"; text: root.controller.directory.replace(/\\/g, "/").split("/").pop(); elide: Text.ElideMiddle; color: Theme.muted }
                Item { visible: root.currentPage !== "assets"; Layout.fillWidth: true }
                Tool { objectName: "globalFocusPreview"; visible: root.currentPage === "assets"; iconName: "fullscreen"; checked: root.focusMode; text: root.focusMode ? Theme.t("退出专注 · Esc", "Exit focus · Esc", "退出專注 · Esc") : Theme.t("专注预览", "Focus preview", "專注預覽"); onClicked: root.setFocusMode(!root.focusMode) }
                Tool { objectName: "globalOpenFolderButton"; visible: root.currentPage === "assets"; iconName: "assets"; primary: true; text: Theme.t("打开目录", "Open folder", "開啟資料夾"); onClicked: folderDialog.open() }
                Tool {
                    objectName: "applicationMenuButton"; iconName: "more"; quiet: true; hint: Theme.t("应用菜单", "Application menu", "應用程式選單")
                    onClicked: mainMenu.open()
                    Popup {
                        id: mainMenu; y: parent.height; x: Math.min(0, parent.width - width); padding: 8; focus: true
                        background: Rectangle { color: Theme.raised; radius: Theme.groupRadius; border.color: Theme.line }
                        contentItem: AppMenu {
                            controller: root.controller
                            onOpenFile: { mainMenu.close(); fileDialog.open() }
                            onToggleFiles: { mainMenu.close(); root.toggleFiles() }
                            onToggleFocus: { mainMenu.close(); root.setFocusMode(!root.focusMode) }
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
                ColumnLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
                    FolderNavigation {
                        id: folderNavigation; controller: root.controller
                        Layout.fillWidth: true; visible: !root.focusMode
                        onBeforeNavigate: { if (drawer.visible) drawerFiles.remember(); else files.remember() }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
                Rectangle {
                    Layout.fillHeight: true; Layout.preferredWidth: 40; color: Theme.panel
                    visible: !root.focusMode && (root.narrow || !root.filesVisible)
                    Tool { id: fileRail; objectName: "fileRail"; width: 38; iconName: "files-open"; hint: Theme.t("展开文件区", "Show files", "展開檔案區"); Accessible.name: hint; onClicked: root.openAddress() }
                }
                Tool {
                    id: focusFiles
                    objectName: "focusFilesRail"; visible: root.focusMode
                    Layout.fillHeight: true; Layout.preferredWidth: 36
                    text: Theme.t("文件", "Files", "檔案")
                    hint: Theme.t("退出专注并打开文件区", "Exit focus and show files", "退出專注並開啟檔案區")
                    contentItem: Item {
                        Text { anchors.centerIn: parent; width: Theme.language === "en" ? 120 : font.pixelSize; text: focusFiles.text; font: focusFiles.font; color: Theme.text; wrapMode: Text.WrapAnywhere; horizontalAlignment: Text.AlignHCenter; rotation: Theme.language === "en" ? -90 : 0 }
                    }
                    onClicked: root.openAddress()
                }
                FilesPane {
                    id: files
                    Layout.fillHeight: true; Layout.preferredWidth: Math.max(220,Math.min(400, root.controller.preferences.fileWidth || Theme.filesWidth))
                    visible: !root.narrow && !root.focusMode && root.filesVisible
                    controller: root.controller; onCollapse: root.toggleFiles(); onEditAddress: folderNavigation.addressFocus()
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
                    onOpenFile: fileDialog.open(); onLocate: locateDialog.open()
                    onToggleFocus: { root.conversionVisible = false; root.setFocusMode(!root.focusMode) }
                    onUnavailable: about.open()
                }
                    }
                }
                Tool {
                    id: focusInspector
                    objectName: "focusInspectorRail"; visible: root.focusMode
                    Layout.fillHeight: true; Layout.preferredWidth: 36
                    text: Theme.t("检查", "Inspect", "檢查")
                    hint: Theme.t("退出专注并显示检查区", "Exit focus and show inspector", "退出專注並顯示檢查區")
                    contentItem: Item {
                        Text { anchors.centerIn: parent; width: Theme.language === "en" ? 120 : font.pixelSize; text: focusInspector.text; font: focusInspector.font; color: Theme.text; wrapMode: Text.WrapAnywhere; horizontalAlignment: Text.AlignHCenter; rotation: Theme.language === "en" ? -90 : 0 }
                    }
                    onClicked: {
                        root.setFocusMode(false)
                        if (!root.inspectorVisible) root.controller.preference("inspectorMode", "always")
                        Qt.callLater(() => inspector.forceActiveFocus())
                    }
                }
                Inspector { id: inspector; Layout.fillHeight: true; Layout.preferredWidth: root.narrow ? Theme.narrowInspectorWidth : Theme.inspectorWidth; visible: root.inspectorVisible; controller: root.controller }
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
        Label { visible: root.settingsVisible && !root.narrow; anchors.left: parent.left; anchors.leftMargin: 24; anchors.verticalCenter: parent.verticalCenter; text: "FormaView"; color: Theme.muted; font.pixelSize: 12 }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: root.settingsVisible ? root.navigationWidth + 24 : 24; anchors.rightMargin: 24; spacing: 12
            AppIcon { name: root.settingsDirty ? "warning" : root.controller.busy ? "loading" : "success"; size: 16; variant: "muted" }
            Label { objectName: "settingsDraftStatus"; color: root.settingsDirty ? Theme.warning : Theme.muted; font.pixelSize: 12; text: root.settingsDirty ? Theme.t("有未保存的更改", "Unsaved changes", "有未儲存的變更") : root.controller.busy ? Theme.t("正在扫描目录…", "Scanning folder…", "正在掃描資料夾…") : Theme.t("就绪", "Ready", "就緒") }
            Item { Layout.fillWidth: true }
            Label { color: Theme.muted; font.pixelSize: 12; text: root.controller.activeTasks ? Theme.t("活动任务 · ", "Active tasks · ", "活動工作 · ") + root.controller.activeTasks : Theme.t("暂无活动任务", "No active tasks", "暫無活動工作") }
        }
    }
    Drawer {
        id: drawer
        objectName: "fileDrawer"
        width: Math.min(460, root.width - 24); height: root.height; modal: true; focus: true
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
        contentItem: FocusScope {
            focus: true
            Keys.onPressed: event => {
                if (root.editing() || event.modifiers !== Qt.AltModifier) return
                if (![Qt.Key_Left, Qt.Key_Right, Qt.Key_Up].includes(event.key)) return
                drawerFiles.remember()
                if (event.key === Qt.Key_Up) root.controller.parentDirectory()
                else root.controller.history(event.key === Qt.Key_Left ? -1 : 1)
                event.accepted = true
            }
            ColumnLayout {
                anchors.fill: parent; spacing: 8
                FolderNavigation { id: drawerNavigation; controller: root.controller; Layout.fillWidth: true; onBeforeNavigate: drawerFiles.remember() }
                FilesPane { id: drawerFiles; Layout.fillWidth: true; Layout.fillHeight: true; controller: root.controller; onCollapse: drawer.close(); onEditAddress: drawerNavigation.addressFocus() }
            }
        }
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
        Label { width: parent.width; wrapMode: Text.WordWrap; color: Theme.text; text: (root.controller.imageJobs ? Theme.t("支持图片通道、像素读数、子资源与批量转换。MCP 默认关闭，需在设置中应用范围并启用。动画播放尚未提供。", "Image channels, pixel values, subresources and batch conversion are available. MCP is off by default; apply a scope and enable it in Settings. Animation playback is not yet available.", "支援影像通道、像素讀數、子資源與批次轉換。MCP 預設關閉，須在設定套用範圍並啟用。動畫播放尚未提供。") : Theme.t("此构建提供文件浏览、缩略图、读取检查与任务中心。未编入图片服务。", "This build provides file browsing, thumbnails, access checks and task status. The image service is not included.", "此組建提供檔案瀏覽、縮圖、讀取檢查與工作中心。未編入影像服務。")) + (root.controller.model ? Theme.t("\n支持 FBX/PSK 模型、骨骼、UV 与源权重只读检查。", "\nRead-only FBX/PSK model, skeleton, UV and source-weight inspection is available.", "\n支援 FBX/PSK 模型、骨骼、UV 與來源權重唯讀檢查。") : "") + Theme.t("\n\nCtrl+O 打开文件 · Ctrl+L 编辑位置\nAlt+← 后退 · Alt+→ 前进 · Alt+↑ 上级\n文件列表内 ↑/↓ 连续浏览\nCtrl / Shift 多选 · Esc 关闭弹层", "\n\nCtrl+O Open · Ctrl+L Location\nAlt+Left Back · Alt+Right Forward · Alt+Up Parent\nUp/Down Browse in file list\nCtrl / Shift Multi-select · Esc Close popup", "\n\nCtrl+O 開啟檔案 · Ctrl+L 編輯位置\nAlt+← 返回 · Alt+→ 前進 · Alt+↑ 上層\n檔案清單內 ↑/↓ 連續瀏覽\nCtrl / Shift 多選 · Esc 關閉彈層") }
    }
    DropArea { anchors.fill: parent; enabled: !root.conversionVisible && !root.settingsVisible; onDropped: drop => { if(drop.hasUrls) { root.controller.openUrls(drop.urls); drop.acceptProposedAction() } } }
}
