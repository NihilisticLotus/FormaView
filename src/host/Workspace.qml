import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
Item {
    id: root
    objectName: "workspace"
    required property var controller
    property bool focusMode: false
    property bool tasksVisible: false
    property bool filesVisible: controller.preferences.filesVisible !== false
    readonly property bool narrow: width <= 1300
    function hasFocusWithin(pane) { if (!pane) return false; for (let item = root.Window.window.activeFocusItem; item; item = item.parent) if (item === pane) return true; return false }
    function returnFileFocus() { Qt.callLater(() => { if (root.tasksVisible) return; if (fileRail.visible) fileRail.forceActiveFocus(); else if (files.visible) files.addressFocus(); else preview.focusLayoutControl() }) }
    function openAddress() {
        tasksVisible = false; focusMode = false; filesVisible = true
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
    ColumnLayout {
        anchors.fill: parent; spacing: 4
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 44; spacing: 4
            Label { Layout.leftMargin: 12; text: "◇  FormaView"; color: Theme.text; font.bold: true }
            AppMenu {
                controller: root.controller
                onOpenFile: fileDialog.open(); onOpenFolder: folderDialog.open()
                onToggleFiles: root.toggleFiles(); onToggleFocus: root.focusMode = !root.focusMode
                onTasks: root.tasksVisible = true; onSettings: settings.open(); onUnavailable: about.open()
            }
            Item { Layout.fillWidth: true }
            Tool { objectName: "tasksButton"; text: Theme.t("任务", "Tasks", "工作") + (root.controller.activeTasks ? " · "+root.controller.activeTasks : ""); checked: root.tasksVisible; onClicked: root.tasksVisible = !root.tasksVisible }
            ComboBox { Layout.preferredWidth: 120; model: ["简体中文","English","繁體中文"]; currentIndex: ["zh-CN","en","zh-TW"].indexOf(root.controller.language); onActivated: root.controller.language = ["zh-CN","en","zh-TW"][currentIndex]; Accessible.name: "Language / 语言" }
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            RowLayout {
                anchors.fill: parent; spacing: 4; visible: !root.tasksVisible
                Rectangle {
                    Layout.fillHeight: true; Layout.preferredWidth: 40; color: Theme.panel
                    visible: root.focusMode || root.narrow || !root.filesVisible
                    Tool { id: fileRail; objectName: "fileRail"; width: 38; text: "≡"; hint: Theme.t("展开文件区", "Show files", "展開檔案區"); Accessible.name: hint; onClicked: root.openAddress() }
                }
                FilesPane {
                    id: files
                    Layout.fillHeight: true; Layout.preferredWidth: Math.max(240,Math.min(440, root.controller.preferences.fileWidth || 268))
                    visible: !root.narrow && !root.focusMode && root.filesVisible
                    controller: root.controller; onCollapse: root.toggleFiles()
                }
                Rectangle {
                    visible: files.visible; Layout.fillHeight: true; Layout.preferredWidth: 4; color: mouse.containsMouse ? Theme.accent : Theme.base
                    MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.SplitHCursor; property real start: 0; property real old: 0
                        onPressed: mouse => { start = mapToItem(root,mouse.x,0).x; old = files.width }
                        onPositionChanged: mouse => { if(pressed) root.controller.preference("fileWidth",Math.max(240, Math.min(440,old+mapToItem(root,mouse.x,0).x-start))) }
                    }
                }
                PreviewPane { id: preview; Layout.fillWidth: true; Layout.fillHeight: true; controller: root.controller; focused: root.focusMode; onOpenFile: fileDialog.open(); onOpenFolder: folderDialog.open(); onLocate: locateDialog.open(); onToggleFocus: root.focusMode = !root.focusMode; onUnavailable: about.open() }
                Inspector { Layout.fillHeight: true; Layout.preferredWidth: root.narrow ? 300 : 320; visible: !root.focusMode; controller: root.controller }
                Rectangle { visible: root.focusMode; Layout.fillHeight: true; Layout.preferredWidth: 40; color: Theme.panel; Tool { width: 38; text: "‹"; hint: Theme.t("展开检查区", "Show inspector", "展開檢查區"); Accessible.name: hint; onClicked: root.focusMode = false } }
            }
            TasksPane { anchors.fill: parent; visible: root.tasksVisible; controller: root.controller; onBack: root.tasksVisible = false }
        }
        Label { Layout.fillWidth: true; Layout.leftMargin: 12; Layout.preferredHeight: 24; color: Theme.muted; font.pixelSize: 12; text: root.controller.busy ? Theme.t("正在扫描目录…", "Scanning folder…", "正在掃描資料夾…") : Theme.t("就绪", "Ready", "就緒") }
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
    SettingsDialog { id: settings; controller: root.controller }
    Dialog {
        id: about; anchors.centerIn: Overlay.overlay; width: 480; modal: true; focus: true; standardButtons: Dialog.Close
        title: "FormaView"
        Label { width: parent.width; wrapMode: Text.WordWrap; color: Theme.text; text: Theme.t("文件浏览、缩略图、读取检查与任务中心已接入。专业预览、外部动画、转换与 MCP 服务将在对应阶段开放。\n\nCtrl+O 打开文件 · Ctrl+L 编辑位置\nAlt+方向键 导航 · 方向键连续浏览\nCtrl / Shift 多选 · Esc 关闭弹层", "File browsing, thumbnails, access checks and task status are available. Professional preview, animation, conversion and MCP are not connected yet.\n\nCtrl+O Open · Ctrl+L Location\nAlt+Arrows Navigate · Arrows Browse\nCtrl / Shift Multi-select · Esc Close popup", "檔案瀏覽、縮圖、讀取檢查與工作中心已接入。專業預覽、外部動畫、轉換與 MCP 服務將於對應階段開放。\n\nCtrl+O 開啟檔案 · Ctrl+L 編輯位置\nAlt+方向鍵 導覽 · 方向鍵連續瀏覽\nCtrl / Shift 多選 · Esc 關閉彈層") }
    }
    DropArea { anchors.fill: parent; onDropped: drop => { if(drop.hasUrls) { root.controller.openUrls(drop.urls); drop.acceptProposedAction() } } }
}
