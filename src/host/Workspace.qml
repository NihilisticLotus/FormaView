import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
Item {
    id: root
    objectName: "workspace"
    required property var controller
    property bool browsing: true
    readonly property string openedPath: controller.asset.path || ""
    property var openedAssets: []
    onOpenedPathChanged: {
        if (!openedPath) return
        browsing = false
        if (!openedAssets.includes(openedPath)) openedAssets = openedAssets.concat([openedPath])
        Qt.callLater(() => assetTabs.positionViewAtIndex(openedAssets.indexOf(openedPath), ListView.Contain))
    }
    function closeAssetTab(path: string): void {
        const index = openedAssets.indexOf(path)
        if (index < 0) return
        const remaining = openedAssets.filter(tab => tab !== path)
        openedAssets = remaining
        if (path !== (openedPath || controller.requested) && remaining.length) {
            // Supersede a tab selection whose inspection has not finished yet.
            if (path === controller.requested && openedPath) controller.openPath(openedPath)
            return
        }
        controller.clearAsset()
        if (remaining.length) controller.openPath(remaining[Math.min(index, remaining.length - 1)])
        else {
            setFocusMode(false)
            navigate("browse")
        }
    }
    readonly property string currentPage: settingsVisible ? "settings" : workspacePage
    readonly property string workspacePage: conversionVisible ? "conversion" : tasksVisible ? "tasks" : browsing ? "browse" : "assets"
    Binding { target: Theme; property: "light"; value: !root.videoWorkspace && root.controller.preferences.appearance !== "dark" }
    readonly property bool videoWorkspace: currentPage === "assets" && controller.asset.kind === "video"
    onCurrentPageChanged: { if (currentPage !== "assets" && preview) preview.pauseSession() }
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
    readonly property bool inspectorVisible: width >= 940 && !focusMode && (controller.preferences.inspectorMode || "auto") !== "hidden" && (controller.preferences.inspectorMode === "always" || !!controller.asset.path)
    property bool exitConfirmed: false
    function requestWindowClose(): bool {
        if (settingsVisible && settingsDialog.dirty) {
            settingsDialog.leave(() => root.Window.window.close())
            return false
        }
        if (root.exitConfirmed) { root.exitConfirmed = false; return true }
        if ((controller.imageJobs && (controller.imageJobs.busy || controller.imageJobs.activeJobs()))
                || (quickExportLoader.item && quickExportLoader.item.settling)) {
            exitDialog.open()
            return false
        }
        return true
    }
    function openConversion(page, preset) { showConversion(page, preset, false) }
    function showConversion(page, preset, newDraft) {
        if (!controller.imageJobs) return
        if (quickExportLoader.item && (quickExportLoader.item.visible || quickExportLoader.item.settling)) { quickExportLoader.item.raise(); quickExportLoader.item.requestActivate(); return }
        if (settingsVisible) { settingsDialog.leave(() => root.showConversion(page, preset, newDraft)); return }
        tasksVisible = false; settingsVisible = false
        if (drawer.visible) drawer.close()
        conversionVisible = true
        if (!conversionLoader.item) conversionLoader.setSource("ConversionWorkspace.qml", {controller: root.controller})
        const workspace = conversionLoader.item as ConversionWorkspace
        if (workspace) {
            if (newDraft) workspace.initialized = false
            workspace.open(page, preset || "")
        }
    }
    function openQuickExport(preset) {
        if (!controller.imageJobs || !controller.asset.path) return
        if (quickExportLoader.item && (quickExportLoader.item.visible || quickExportLoader.item.settling)) { quickExportLoader.item.raise(); quickExportLoader.item.requestActivate(); return }
        quickExportLoader.setSource("", {})
        // A geometry-cache frame export binds the presented sample at open time.
        const cache = preset === "abcFrame" && controller.model ? controller.model.cacheInfo : ({})
        if (preset === "abcFrame" && cache.sample === undefined) return
        quickExportLoader.setSource("QuickExportWindow.qml", {controller: root.controller, inputPath: String(controller.asset.path), preset: preset,
            abcSample: preset === "abcFrame" ? cache.sample : -1, abcSeconds: preset === "abcFrame" ? cache.time : 0,
            abcSourceSha256: preset === "abcFrame" ? String(controller.model.info.sha256 || "") : "", transientParent: root.Window.window})
    }
    readonly property bool narrow: width <= 1080
    function hasFocusWithin(pane) { if (!pane) return false; for (let item = root.Window.window.activeFocusItem; item; item = item.parent) if (item === pane) return true; return false }
    function returnFileFocus() { Qt.callLater(() => { if (root.tasksVisible) return; if (fileRail.visible) fileRail.forceActiveFocus(); else if (files.visible) files.addressFocus(); else preview.focusLayoutControl() }) }
    function openAddress() {
        if (currentPage === "browse") { library.addressFocus(); return }
        if (settingsVisible) { settingsDialog.leave(() => root.openAddress()); return }
        settingsVisible = false; conversionVisible = false; tasksVisible = false; focusMode = false; filesVisible = true
        if (videoWorkspace) { drawer.open(); if (drawer.opened) drawerFiles.addressFocus(); return }
        controller.preference("filesVisible", true)
        if (narrow) { drawer.open(); if (drawer.opened) drawerFiles.addressFocus() }
        else Qt.callLater(() => folderNavigation.addressFocus())
    }
    function toggleFiles() {
        if (currentPage === "browse") { library.sidebarVisible = !library.sidebarVisible; return }
        if (narrow || videoWorkspace) { if (drawer.visible) drawer.close(); else openAddress(); return }
        if (focusMode || !filesVisible) { openAddress(); return }
        files.remember(); filesVisible = false; controller.preference("filesVisible", false); returnFileFocus()
    }
    onFocusModeChanged: { if (drawer.visible) drawer.close(); Qt.callLater(() => { if (!root.tasksVisible) preview.focusLayoutControl() }) }
    onNarrowChanged: { if (narrow && hasFocusWithin(files)) returnFileFocus(); else if (!narrow && !videoWorkspace && drawer.visible) drawer.close() }
    function editing(): bool { const item = root.Window.window.activeFocusItem; return !!item && (item.hasOwnProperty("inputMethodComposing") || item.hasOwnProperty("selectByMouse")) }
    Shortcut { sequence: "Ctrl+O"; enabled: !root.settingsVisible; onActivated: fileDialog.present() }
    Shortcut { sequence: "Ctrl+L"; enabled: !root.settingsVisible; onActivated: root.openAddress() }
    Shortcut { sequence: "Alt+Left"; enabled: !root.settingsVisible && !root.editing(); onActivated: root.controller.history(-1) }
    Shortcut { sequence: "Alt+Right"; enabled: !root.settingsVisible && !root.editing(); onActivated: root.controller.history(1) }
    Shortcut { sequence: "Alt+Up"; enabled: !root.settingsVisible && !root.editing(); onActivated: root.controller.parentDirectory() }
    Connections {
        target: root.controller
        function onExplorerFileOpened() { root.navigate("assets") }
    }
    function navigate(destination): void {
        if (["general", "preview", "cache", "permissions", "about"].includes(destination)) { settingsDialog.section = destination; return }
        if (destination === "settings") { if (!settingsVisible) settingsDialog.open("general"); return }
        if (settingsVisible) { settingsDialog.leave(() => root.navigate(destination)); return }
        if (drawer.visible) drawer.close()
        if (destination === "conversion" && !conversionLoader.item) { openConversion(0); return }
        conversionVisible = destination === "conversion"
        tasksVisible = destination === "tasks"
        if (destination === "browse" || destination === "assets") browsing = destination === "browse"
    }
    Item {
        id: workspaceSurface
        anchors.fill: parent
        enabled: !root.settingsVisible
        layer.enabled: root.settingsVisible || (root.Window.window?.frostDepth ?? 0) > 0 || drawer.visible || compactInspector.visible
        layer.effect: FrostEffect {}
    Canvas {
        id: wallpaper
        visible: !root.videoWorkspace
        // A smooth, static light field needs one small raster per theme. The
        // side panels transmit this layer without sampling any live viewport.
        width: 384; height: 256; smooth: true
        transform: Scale { xScale: root.width / wallpaper.width; yScale: root.height / wallpaper.height }
        readonly property bool light: Theme.light
        onLightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.save()
            ctx.scale(width, height)
            const base = ctx.createLinearGradient(0, 0, 1, .75)
            base.addColorStop(0, light ? "#ccdeed" : "#263a4b")
            base.addColorStop(.38, light ? "#e4edf5" : "#242d38")
            base.addColorStop(.7, light ? "#aec8d9" : "#334b5b")
            base.addColorStop(1, light ? "#e5dcd2" : "#393c3f")
            ctx.fillStyle = base
            ctx.fillRect(0, 0, 1, 1)
            const warm = ctx.createRadialGradient(1.05, .9, 0, 1.05, .9, .69)
            warm.addColorStop(0, light ? "rgba(209,180,148,.65)" : "rgba(154,120,81,.32)")
            warm.addColorStop(1, "rgba(209,180,148,0)")
            ctx.fillStyle = warm
            ctx.fillRect(0, 0, 1, 1)
            const blue = ctx.createRadialGradient(-.08, .52, 0, -.08, .52, .71)
            blue.addColorStop(0, light ? "rgba(135,184,210,.73)" : "rgba(71,116,142,.55)")
            blue.addColorStop(1, "rgba(135,184,210,0)")
            ctx.fillStyle = blue
            ctx.fillRect(0, 0, 1, 1)
            ctx.restore()
        }
    }
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: root.videoWorkspace ? 42 : Theme.topBarHeight; color: root.videoWorkspace ? Theme.base : "transparent"
            MouseArea {
                id: titleDrag
                objectName: "windowTitleDrag"
                anchors.fill: parent; anchors.rightMargin: Math.max(160, root.Window.window.nativeCaptionInset)
                onPressed: root.Window.window.startSystemMove()
                onDoubleClicked: {
                    const window = root.Window.window
                    if (window.visibility === Window.Maximized) window.showNormal()
                    else window.showMaximized()
                }
            }
            RowLayout {
                objectName: "topBar"; anchors.fill: parent; anchors.leftMargin: 24; anchors.rightMargin: Math.max(160, root.Window.window.nativeCaptionInset); spacing: 12
                AppIcon { name: "brand-diamond"; size: 24 }
                Label { visible: root.width > 940; text: "FormaView"; color: Theme.text; font.pixelSize: 17; font.bold: true }
                Tool {
                    objectName: "browseToggleFiles"; visible: root.currentPage === "browse"; quiet: true
                    iconName: library.sidebarVisible ? "files-close" : "files-open"
                    hint: library.sidebarVisible ? Theme.t("收起文件栏","Hide file sidebar","收起檔案欄") : Theme.t("展开文件栏","Show file sidebar","展開檔案欄")
                    onClicked: root.toggleFiles()
                }
                Tool { iconName: "folder"; quiet: true; text: root.width > 1100 ? Theme.t("打开","Open","開啟") : ""; hint: Theme.t("打开文件","Open file","開啟檔案"); onClicked: fileDialog.present() }
                Item { Layout.fillWidth: true }
                Rectangle {
                    readonly property bool captionClient: true
                    radius: height/2; color: Theme.glass; border.color: Theme.glassEdge
                    implicitWidth: tabs.implicitWidth + 12; implicitHeight: tabs.implicitHeight + 8
                    Row {
                        id: tabs; anchors.centerIn: parent; spacing: 3
                        Repeater {
                            model: ["browse","assets","conversion","tasks"]
                            Tool {
                                required property string modelData
                                objectName: "workspaceTab-" + modelData
                                iconName: root.width < 980 ? "" : ({"browse":"folder","assets":"preview","conversion":"conversion","tasks":"tasks"})[modelData]
                                background: Rectangle {
                                    radius: height/2; color: parent.checked ? (Theme.light ? "#e9ffffff" : "#8153697a") : parent.hovered ? Theme.hover : "transparent"
                                    border.color: parent.visualFocus ? Theme.accent : "transparent"
                                }
                                checked: root.workspacePage === modelData; quiet: !checked
                                enabled: modelData !== "conversion" || !!root.controller.imageJobs
                                text: modelData === "browse" ? Theme.t("浏览","Browse","瀏覽") : modelData === "assets" ? Theme.t("检查","Inspect","檢查") : modelData === "conversion" ? Theme.t("转换","Convert","轉換") : Theme.t("任务","Tasks","工作")
                                onClicked: root.navigate(modelData)
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                Tool {
                    objectName: "quickAppearance"
                    visible: !root.videoWorkspace
                    quiet: true; implicitWidth: 32
                    hint: Theme.light ? Theme.t("切换到深色玻璃","Switch to dark glass","切換到深色玻璃") : Theme.t("切换到浅银玻璃","Switch to silver glass","切換到淺銀玻璃")
                    contentItem: Item {
                        Rectangle {
                            anchors.centerIn: parent; width: 17; height: 17; radius: 9; color: "transparent"; border.color: Theme.text; border.width: 1.5
                            Item { width: 8.5; height: 17; clip: true
                                Rectangle { width: 17; height: 17; radius: 9; color: Theme.text }
                            }
                        }
                    }
                    onClicked: root.controller.preference("appearance", Theme.light ? "dark" : "light")
                }
                Tool { id: settingsButton; objectName: "workspaceSettings"; iconName: "settings"; quiet: true; hint: Theme.t("设置","Settings","設定"); onClicked: root.navigate("settings") }
                Tool { objectName: "globalFocusPreview"; visible: root.currentPage === "assets" && !root.videoWorkspace; iconName: "fullscreen"; quiet: true; checked: root.focusMode; hint: Theme.t("专注预览","Focus view","專注預覽"); onClicked: root.setFocusMode(!root.focusMode) }
                Tool {
                    objectName: "inspectorToggle"; visible: root.currentPage === "assets" && !root.videoWorkspace
                    quiet: true; iconName: "inspector-open"; hint: Theme.t("检查器","Inspector","檢查器")
                    onClicked: {
                        if (root.width < 940) { compactInspector.open(); return }
                        root.controller.preference("inspectorMode", root.inspectorVisible ? "hidden" : "always")
                    }
                }
                Tool { objectName: "videoBrowseFiles"; visible: root.videoWorkspace; quiet: true; iconName: "files-open"; hint: Theme.t("浏览文件", "Browse files", "瀏覽檔案"); onClicked: root.toggleFiles() }
                Tool {
                    objectName: "applicationMenuButton"; iconName: "more"; quiet: true; hint: Theme.t("应用菜单", "Application menu", "應用程式選單")
                    onClicked: mainMenu.open()
                    Popup {
                        id: mainMenu; y: parent.height; x: Math.min(0, parent.width - width); padding: 8; focus: true
                        background: Rectangle { color: Theme.raised; radius: Theme.groupRadius; border.color: Theme.line }
                        contentItem: AppMenu {
                            controller: root.controller
                            onCloseCurrentFile: root.closeAssetTab(root.openedPath)
                            onOpenFile: { mainMenu.close(); fileDialog.present() }
                            onToggleFiles: { mainMenu.close(); root.toggleFiles() }
                            onToggleFocus: { mainMenu.close(); root.setFocusMode(!root.focusMode) }
                            onTasks: { mainMenu.close(); root.navigate("tasks") }
                            onSettings: { mainMenu.close(); settingsDialog.open("permissions") }
                            onUnavailable: { mainMenu.close(); about.open() }
                            onConvert: { mainMenu.close(); root.openConversion(0) }
                            onAddAnimation: { mainMenu.close(); preview.addExternalAnimation() }
                        }
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
        }
        RowLayout {
            visible: !root.videoWorkspace && !root.focusMode && (root.workspacePage === "browse" || root.workspacePage === "assets")
            Layout.fillWidth: true; Layout.minimumHeight: 37; Layout.preferredHeight: 37; Layout.maximumHeight: 37; Layout.leftMargin: 16; Layout.rightMargin: 16; spacing: 12
            Tool {
                iconName: "folder"; quiet: true; Layout.maximumWidth: 218
                text: root.controller.directory.split("/").pop() || Theme.t("工作区","Workspace","工作區")
                hint: Theme.t("打开目录","Open folder","開啟目錄"); onClicked: folderDialog.present()
            }
            ListView {
                id: assetTabs; orientation: ListView.Horizontal; clip: true; spacing: 4
                Layout.fillWidth: true; Layout.fillHeight: true; model: root.openedAssets
                delegate: Item {
                    id: tab
                    required property string modelData
                    readonly property string label: modelData.split("/").pop()
                    readonly property bool showClose: hover.hovered || selectTab.visualFocus || closeTab.visualFocus
                    width: Math.min(194, selectTab.implicitWidth)
                    height: selectTab.implicitHeight
                    y: (assetTabs.height - height) / 2
                    HoverHandler { id: hover; blocking: false }
                    Tool {
                        id: selectTab
                        anchors.fill: parent
                        text: tab.label; quiet: true; checked: root.openedPath === tab.modelData; hint: tab.modelData
                        iconName: /\.(fbx|abc|gltf|glb|obj|psk|pskx|psa)$/i.test(tab.modelData) ? "model" : "file"
                        onClicked: {
                            if (tab.modelData !== root.openedPath || root.controller.requested !== tab.modelData)
                                root.controller.openPath(tab.modelData)
                            else root.navigate("assets")
                        }
                    }
                    Tool {
                        id: closeTab
                        anchors.left: parent.left; anchors.leftMargin: selectTab.leftPadding - 3; anchors.verticalCenter: parent.verticalCenter
                        width: 24; height: 24; leftPadding: 5; rightPadding: 5
                        visible: tab.showClose
                        iconName: "close"
                        hint: Theme.t("关闭标签", "Close tab", "關閉標籤") + " · " + tab.label
                        background: Rectangle { radius: 5; color: closeTab.hovered || closeTab.visualFocus ? Theme.hover : selectTab.checked ? Theme.selection : Theme.raised }
                        onClicked: root.closeAssetTab(tab.modelData)
                    }
                }
            }
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            LibraryWorkspace {
                id: library; objectName: "libraryWorkspace"; anchors.fill: parent; visible: root.workspacePage === "browse"; controller: root.controller
                onChooseDirectory: folderDialog.present()
                onInspect: path => {
                    if (root.openedPath === path) root.navigate("assets")
                    else root.controller.openPath(path)
                }
            }
            RowLayout {
                anchors.fill: parent; anchors.margins: root.videoWorkspace ? 0 : 8; spacing: 8; visible: root.workspacePage === "assets"
                ColumnLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
                    FolderNavigation {
                        id: folderNavigation; controller: root.controller
                        Layout.fillWidth: true; visible: !root.focusMode && !root.videoWorkspace
                        onBeforeNavigate: { if (drawer.visible) drawerFiles.remember(); else files.remember() }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
                Rectangle {
                    Layout.fillHeight: true; Layout.preferredWidth: 40; color: Theme.panel
                    visible: !root.videoWorkspace && !root.focusMode && (root.narrow || !root.filesVisible)
                    Tool { id: fileRail; objectName: "fileRail"; width: 38; iconName: "files-open"; hint: Theme.t("展开文件区", "Show files", "展開檔案區"); Accessible.name: hint; onClicked: root.openAddress() }
                }
                Tool {
                    id: focusFiles
                    objectName: "focusFilesRail"; visible: root.focusMode && !root.videoWorkspace
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
                    onBrowseCollection: key => { root.navigate("browse"); library.choose(key) }
                    Layout.fillHeight: true; Layout.preferredWidth: splitterMouse.pressed ? splitterMouse.dragWidth : Math.max(220,Math.min(400, root.controller.preferences.fileWidth || Theme.filesWidth))
                    visible: !root.videoWorkspace && !root.narrow && !root.focusMode && root.filesVisible
                    controller: root.controller; onCollapse: root.toggleFiles(); onEditAddress: folderNavigation.addressFocus()
                }
                Rectangle {
                    objectName: "filesPaneSplitterHitArea"
                    visible: files.visible; Layout.fillHeight: true; Layout.preferredWidth: 10
                    Layout.leftMargin: -4; Layout.rightMargin: -4; color: "transparent"
                    Rectangle {
                        objectName: "filesPaneSplitterVisual"
                        anchors.horizontalCenter: parent.horizontalCenter; width: 4; height: parent.height
                        color: splitterMouse.containsMouse ? Theme.accent : "transparent"
                    }
                    MouseArea { id: splitterMouse; objectName: "filesPaneSplitterMouseArea"; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.SplitHCursor; property real start: 0; property real old: 0; property real dragWidth: 300
                        onPressed: event => { splitterMouse.start = splitterMouse.mapToItem(root,event.x,0).x; splitterMouse.old = files.width; splitterMouse.dragWidth = files.width }
                        onPositionChanged: event => { if(splitterMouse.pressed) splitterMouse.dragWidth = Math.max(220, Math.min(400,splitterMouse.old+splitterMouse.mapToItem(root,event.x,0).x-splitterMouse.start)) }
                        onReleased: event => { root.controller.preference("fileWidth", splitterMouse.dragWidth) }
                    }
                }
                PreviewPane {
                    id: preview; Layout.fillWidth: true; Layout.fillHeight: true; controller: root.controller; focused: root.focusMode
                    onCloseCurrentFile: root.closeAssetTab(root.openedPath)
                    onOpenFile: fileDialog.present(); onLocate: locateDialog.present()
                    onToggleFocus: { root.conversionVisible = false; root.setFocusMode(!root.focusMode) }
                    onUnavailable: about.open()
                    onConvert: root.openConversion(0)
                    onExportShortcut: preset => root.openQuickExport(preset)
                    onInspectModelSection: section => {
                        if (inspector.visible) inspector.inspectSection(section)
                        else if (!["model", "uv"].includes(section)) {
                            compactInspector.requestedSection = section
                            compactInspector.open()
                        }
                    }
                }
                    }
                }
                Tool {
                    id: focusInspector
                    objectName: "focusInspectorRail"; visible: root.focusMode && !root.videoWorkspace
                    Layout.fillHeight: true; Layout.preferredWidth: 36
                    text: Theme.t("检查", "Inspect", "檢查")
                    hint: Theme.t("退出专注并显示检查区", "Exit focus and show inspector", "退出專注並顯示檢查區")
                    contentItem: Item {
                        Text { anchors.centerIn: parent; width: Theme.language === "en" ? 120 : font.pixelSize; text: focusInspector.text; font: focusInspector.font; color: Theme.text; wrapMode: Text.WrapAnywhere; horizontalAlignment: Text.AlignHCenter; rotation: Theme.language === "en" ? -90 : 0 }
                    }
                    onClicked: {
                        root.setFocusMode(false)
                        if (root.width < 940) { compactInspector.open(); return }
                        if (!root.inspectorVisible) root.controller.preference("inspectorMode", "always")
                        Qt.callLater(() => inspector.forceActiveFocus())
                    }
                }
                Inspector { id: inspector; Layout.fillHeight: true; Layout.preferredWidth: root.narrow ? Theme.narrowInspectorWidth : Theme.inspectorWidth; visible: root.inspectorVisible && !root.videoWorkspace; controller: root.controller }
            }
            TasksPane { anchors.fill: parent; visible: root.tasksVisible && !root.conversionVisible; controller: root.controller; onBack: root.navigate("assets"); onNewConversion: root.showConversion(0, "", true); onRetryImageJob: id => { if (quickExportLoader.item && quickExportLoader.item.visible) { quickExportLoader.item.raise(); quickExportLoader.item.requestActivate(); return } root.controller.imageJobs.retry(id); root.openConversion(1) } }
            Loader { id: conversionLoader; anchors.fill: parent; visible: root.conversionVisible }
        }
        Item { Layout.fillWidth: true; Layout.preferredHeight: Theme.statusHeight; visible: !root.videoWorkspace }
    }
    Rectangle {
        objectName: "statusBar"; anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: Theme.statusHeight; color: "transparent"; visible: !root.videoWorkspace
        Rectangle { width: parent.width; height: 1; color: Theme.line }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 24; anchors.rightMargin: 24; spacing: 12
            AppIcon { name: root.settingsDirty ? "warning" : root.controller.busy ? "loading" : "success"; size: 16; variant: "muted" }
            Label { objectName: "settingsDraftStatus"; color: root.settingsDirty ? Theme.warning : Theme.muted; font.pixelSize: 12; text: root.settingsDirty ? Theme.t("有未保存的更改", "Unsaved changes", "有未儲存的變更") : root.controller.busy ? Theme.t("正在扫描目录…", "Scanning folder…", "正在掃描資料夾…") : Theme.t("就绪", "Ready", "就緒") }
            Item { Layout.fillWidth: true }
            Label { color: Theme.muted; font.pixelSize: 12; text: root.controller.activeTasks ? Theme.t("活动任务 · ", "Active tasks · ", "活動工作 · ") + root.controller.activeTasks : Theme.t("暂无活动任务", "No active tasks", "暫無活動工作") }
        }
    }
    }
    Rectangle {
        anchors.fill: parent
        visible: (root.Window.window?.frostDepth ?? 0) > 0 && !root.settingsVisible
        color: Theme.light ? "#57404b56" : "#740a1019"
        Accessible.ignored: true
    }
    SettingsDialog {
        id: settingsDialog; anchors.fill: parent; controller: root.controller; visible: root.settingsVisible
        onAddressRequested: root.openAddress()
        onOpened: root.settingsVisible = true
        onClosed: { root.settingsVisible = false; Qt.callLater(() => settingsButton.forceActiveFocus()) }
    }
    Drawer {
        property string requestedSection: ""
        onClosed: requestedSection = ""
        id: compactInspector; objectName: "compactInspectorDrawer"; edge: Qt.RightEdge; width: Math.min(350,root.width-32); height: root.height
        contentItem: Loader {
            active: compactInspector.visible
            onLoaded: if (compactInspector.requestedSection) item.inspectSection(compactInspector.requestedSection)
            sourceComponent: Component { Inspector { controller: root.controller } }
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
                FilesPane { id: drawerFiles; Layout.fillWidth: true; Layout.fillHeight: true; controller: root.controller; onBrowseCollection: key => { drawer.close(); root.navigate("browse"); library.choose(key) }; onCollapse: drawer.close(); onEditAddress: drawerNavigation.addressFocus() }
            }
        }
        onAboutToHide: drawerFiles.remember()
        onOpened: drawerFiles.addressFocus()
        onClosed: root.returnFileFocus()
    }
    FrostFileDialog { parentWindow: root.Window.window; id: fileDialog; title: Theme.t("打开文件", "Open file", "開啟檔案"); onAccepted: root.controller.openPath(selectedFile.toString()) }
    FrostFolderDialog { parentWindow: root.Window.window; id: folderDialog; title: Theme.t("打开目录", "Open folder", "開啟資料夾"); onAccepted: { root.navigate("browse"); library.choose("directory"); root.controller.navigate(selectedFolder.toString()) } }
    FrostFileDialog { parentWindow: root.Window.window; id: locateDialog; title: Theme.t("定位替代文件 · 读取检查后应用", "Locate replacement · Check before applying", "定位替代檔案 · 讀取檢查後套用"); onAccepted: root.controller.openPath(selectedFile.toString()) }
    Loader { id: quickExportLoader }
    FrostDialog {
        id: exitDialog; objectName: "activeWorkExitDialog"
        property var suspendedExport: null
        anchors.centerIn: Overlay.overlay; width: 520; modal: true; focus: true
        onAboutToShow: {
            const quick = quickExportLoader.item
            if (quick && quick.visible) { suspendedExport = quick; quick.hide() }
        }
        onClosed: {
            const quick = suspendedExport
            suspendedExport = null
            if (quick) { quick.show(); quick.raise(); quick.requestActivate() }
        }
        title: Theme.t("任务仍在运行", "Tasks are still running", "工作仍在執行")
        background: Rectangle { color: Theme.raised; radius: Theme.dialogRadius; border.color: Theme.line }
        Label {
            width: parent.width; wrapMode: Text.WordWrap; color: Theme.text
            text: Theme.t("退出将取消尚未完成的转换任务。", "Exiting will cancel unfinished conversions.", "結束將取消尚未完成的轉換工作。")
        }
        footer: DialogButtonBox {
            Tool { form: true; text: Theme.t("继续运行", "Keep running", "繼續執行"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            Tool { form: true; text: Theme.t("取消并退出", "Cancel and exit", "取消並結束"); DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
            onRejected: exitDialog.reject()
            onAccepted: exitDialog.accept()
        }
        onAccepted: { suspendedExport = null; root.exitConfirmed = true; root.Window.window.close() }
    }
    Connections { target: conversionLoader.item; function onClosed() { root.conversionVisible = false; Qt.callLater(() => settingsButton.forceActiveFocus()) } }
    FrostDialog {
        id: about; anchors.centerIn: Overlay.overlay; width: 520; modal: true; focus: true
        font.pixelSize: Theme.formText
        background: Rectangle { color: Theme.raised; radius: Theme.dialogRadius; border.color: Theme.line }
        footer: DialogButtonBox {
            Tool { form: true; text: Theme.t("关闭", "Close", "關閉"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            onRejected: about.reject()
        }
        title: "FormaView"
        Label {
            width: parent.width; wrapMode: Text.WordWrap; color: Theme.text; textFormat: Text.PlainText
            text: (root.controller.imageJobs
                   ? Theme.t("图片：读取 PNG / JPEG / BMP / TGA / TIFF / WebP / GIF / ICO / DDS / HDR / EXR，查看通道、透明度、曝光与源像素；可写出 PNG / JPEG / BMP / TGA / TIFF / WebP / DDS / HDR / EXR，转换前先预检并确认损失。",
                             "Images: read PNG / JPEG / BMP / TGA / TIFF / WebP / GIF / ICO / DDS / HDR / EXR; inspect channels, alpha, exposure and source pixels; write PNG / JPEG / BMP / TGA / TIFF / WebP / DDS / HDR / EXR. Every conversion is preflighted and losses must be confirmed.",
                             "影像：讀取 PNG / JPEG / BMP / TGA / TIFF / WebP / GIF / ICO / DDS / HDR / EXR，檢視通道、透明度、曝光與來源像素；可寫出 PNG / JPEG / BMP / TGA / TIFF / WebP / DDS / HDR / EXR，轉換前先預檢並確認損失。")
                   : Theme.t("此构建提供文件浏览、缩略图、读取检查与任务中心。未编入图片服务。", "This build provides file browsing, thumbnails, access checks and task status. The image service is not included.", "此組建提供檔案瀏覽、縮圖、讀取檢查與工作中心。未編入影像服務。"))
                  + (root.controller.model
                     ? Theme.t("\n模型：预览 FBX / OBJ / glTF / GLB / PSK / PSKX，检查 UV、完整源权重、骨架与绑定、LOD 与碰撞；播放 FBX / glTF 动作与 PSA（独立或外部配对），可比较骨架、锁定根运动。导出 FBX（含 PMX / VRM 输入）同样先预检再确认损失。",
                               "\nModels: preview FBX / OBJ / glTF / GLB / PSK / PSKX; inspect UV, full source weights, skeleton and bindings, LOD and collision; play FBX / glTF actions and PSA (standalone or paired); compare skeletons and lock root motion. FBX export (including PMX / VRM inputs) is also preflighted with losses confirmed first.",
                               "\n模型：預覽 FBX / OBJ / glTF / GLB / PSK / PSKX，檢查 UV、完整來源權重、骨架與綁定、LOD 與碰撞；播放 FBX / glTF 動作與 PSA（獨立或外部配對），可比較骨架、鎖定根運動。匯出 FBX（含 PMX / VRM 輸入）同樣先預檢再確認損失。")
                     : "")
                  + Theme.t("\nMCP 默认关闭：在设置中应用输入/输出范围后启用，客户端与界面共用同一套预检与任务。",
                            "\nMCP is off by default: apply an input/output scope in settings and enable it. Clients and this window share the same preflight and tasks.",
                            "\nMCP 預設關閉：在設定中套用輸入/輸出範圍後啟用，用戶端與介面共用同一套預檢與工作。")
                  + Theme.t("\n\nCtrl+O 打开文件 · Ctrl+L 编辑位置 · Esc 返回/关闭弹层\nAlt+← 后退 · Alt+→ 前进 · Alt+↑ 上级\n文件列表内 ↑/↓ 连续浏览 · Ctrl / Shift 多选\n图片：F 适合 · Z 原始像素 · 空格 平铺 · C/R/G/B/A 通道\n模型：F 聚焦 · Home 显示全部 · 空格 播放/暂停（有动画时）",
                            "\n\nCtrl+O Open · Ctrl+L Location · Esc Back / close popup\nAlt+Left Back · Alt+Right Forward · Alt+Up Parent\nUp/Down Browse in file list · Ctrl / Shift Multi-select\nImage: F Fit · Z Actual pixels · Space Tile · C/R/G/B/A Channels\nModel: F Focus · Home Frame all · Space Play/Pause (when animated)",
                            "\n\nCtrl+O 開啟檔案 · Ctrl+L 編輯位置 · Esc 返回/關閉彈層\nAlt+← 返回 · Alt+→ 前進 · Alt+↑ 上層\n檔案清單內 ↑/↓ 連續瀏覽 · Ctrl / Shift 多選\n影像：F 符合視窗 · Z 原始像素 · 空白鍵 並排 · C/R/G/B/A 通道\n模型：F 聚焦 · Home 顯示全部 · 空白鍵 播放/暫停（有動畫時）")
        }
    }
    DropArea { anchors.fill: parent; enabled: !root.conversionVisible; onDropped: drop => { if(drop.hasUrls) { if (!preview.subtitleDrop(drop.urls)) root.controller.openUrls(drop.urls); drop.acceptProposedAction() } } }
}
