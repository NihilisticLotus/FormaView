pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
Rectangle {
    id: root
    required property var controller
    property string viewMode: controller.preferences.fileViewMode || (controller.preferences.thumbnails ? "thumbnails" : "list")
    readonly property bool thumbnails: viewMode === "thumbnails"
    readonly property bool tiles: viewMode === "tiles"
    readonly property bool gridMode: thumbnails || tiles
    readonly property int columns: gridMode ? Math.max(1, Math.floor(grid.width / (tiles ? 190 : 128))) : 1
    readonly property var activeView: gridMode ? grid : list
    function sizeText(bytes) {
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1048576) return (bytes / 1024).toFixed(1) + " KiB"
        if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + " MiB"
        return (bytes / 1073741824).toFixed(1) + " GiB"
    }
    signal collapse()
    objectName: "filesPane"
    color: Theme.panel; radius: Theme.groupRadius; border.color: Theme.line
    signal editAddress()
    function addressFocus() { editAddress() }
    function viewFocus() { activeView.forceActiveFocus() }
    Keys.onEscapePressed: event => { root.remember(); root.collapse(); event.accepted = true }
    function remember() { if (!visible) return; controller.saveView(Math.max(0,activeView.contentY-activeView.originY)/(gridMode ? grid.cellHeight : Theme.fileRowHeight)*columns) }
    property bool restorePending: false
    property int restoreStableFrames: 0
    function restoreGeometryChanged() {
        if (!restorePending) return
        restoreStableFrames = 0
        Qt.callLater(applyRestore)
    }
    property bool modelResetting: false
    function finishModelReset() { modelResetting = false }
    Connections {
        target: root.controller.files
        // Cancel pending cache delegates while their old indices are still valid.
        function onModelAboutToBeReset() { root.modelResetting = true }
        // Rebind after all Qt modelReset handlers have finished clearing old items.
        function onModelReset() { Qt.callLater(root.finishModelReset) }
    }
    property real restorePosition: 0
    function applyRestore() {
        if (!visible || modelResetting) return false
        const view = activeView
        view.forceLayout()
        // Reattaching the model can span frames while delegates incubate.
        // Do not finish against an empty or partially measured view.
        const expectedHeight = gridMode ? Math.ceil(controller.count/columns)*grid.cellHeight : controller.count*Theme.fileRowHeight
        if (view.count !== controller.count || Math.abs(view.contentHeight-expectedHeight) > 1) return false
        const offset = restorePosition/columns*(gridMode ? grid.cellHeight : Theme.fileRowHeight)
        view.contentY = Math.max(view.originY, Math.min(view.originY+offset, view.originY+view.contentHeight-view.height))
        return true
    }
    function restore() {
        restorePosition = controller.scrollPosition
        restorePending = true
        restoreStableFrames = 0
        Qt.callLater(applyRestore)
    }
    Connections {
        target: root.Window.window
        enabled: root.visible && root.restorePending
        function onFrameSwapped() {
            if (!root.applyRestore()) { root.restoreStableFrames = 0; return }
            // A stale frame can arrive before visibility-driven layouts finish.
            // Require another rendered frame with unchanged view geometry.
            root.restoreStableFrames += 1
            if (root.restoreStableFrames >= 2) root.restorePending = false
            else root.Window.window.update()
        }
    }
    function step(delta, modifiers) { if (!controller.count) return; const next = Math.max(0, Math.min(controller.count-1, controller.currentIndex+delta)); if (next === controller.currentIndex) return; controller.select(next, modifiers); activeView.positionViewAtIndex(next, ListView.Contain) }
    Connections { target: root.controller; function onViewRestored() { root.restore() } }
    onViewModeChanged: restore()
    onVisibleChanged: if (visible) restore()
    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10; Layout.minimumHeight: 34
            Label { text: Theme.t("文件", "Files", "檔案"); font.pixelSize: Theme.compactText; font.bold: true; color: Theme.text; Layout.fillWidth: true }
            Tool { id: collapseButton; objectName: "collapseFiles"; iconName: "files-close"; quiet: true; Accessible.name: Theme.t("收起文件区", "Collapse files", "收起檔案區"); onClicked: { root.remember(); root.collapse() } }
        }
        Field {
            id: search
            objectName: "search"
            Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
            text: root.controller.query
            Binding { target: search; property: "text"; when: !search.activeFocus; value: root.controller.query }
            placeholderText: Theme.t("搜索文件", "Search files", "搜尋檔案")
            Accessible.name: placeholderText
            onTextEdited: if (!inputMethodComposing) debounce.restart()
            onInputMethodComposingChanged: if (!inputMethodComposing) debounce.restart()
            onAccepted: if (!inputMethodComposing) { debounce.stop(); root.controller.filter(text, root.controller.typeFilter, root.controller.sortOrder) }
            Keys.onEscapePressed: event => { root.viewFocus(); event.accepted = true }
            Timer { id: debounce; interval: 150; onTriggered: if (!search.inputMethodComposing) root.controller.filter(search.text, root.controller.typeFilter, root.controller.sortOrder) }
        }
        Tool {
            id: filterButton
            objectName: "filterButton"
            Layout.maximumWidth: root.width - 20; Layout.leftMargin: 10; Layout.rightMargin: 10
            text: Theme.t("筛选 / 排序", "Filter / Sort", "篩選 / 排序") + " · " + [Theme.t("名称↑", "Name↑", "名稱↑"),Theme.t("名称↓", "Name↓", "名稱↓"),Theme.t("修改时间", "Modified", "修改時間"),Theme.t("大小", "Size", "大小")][root.controller.sortOrder] + (root.controller.typeFilter ? " · " + ["",Theme.t("图片", "Images", "影像"),Theme.t("模型", "Models", "模型"),Theme.t("动画", "Animation", "動畫"),"ABC",Theme.t("全部", "All", "全部")][root.controller.typeFilter] : "")
            hint: text
            onClicked: filters.open()
            AppPopupMenu {
                id: filters
                y: filterButton.height
                AppMenuItem { text: Theme.t("可预览文件（默认）", "Previewable files (default)", "可預覽檔案（預設）"); checkable: true; checked: root.controller.typeFilter === 0; onTriggered: root.controller.filter(search.text, 0, root.controller.sortOrder) }
                AppMenuItem { text: Theme.t("全部类型", "All types", "全部類型"); checkable: true; checked: root.controller.typeFilter === 5; onTriggered: root.controller.filter(search.text, 5, root.controller.sortOrder) }
                AppMenuItem { text: Theme.t("图片 / 纹理", "Images / Textures", "影像 / 紋理"); checkable: true; checked: root.controller.typeFilter === 1; onTriggered: root.controller.filter(search.text, 1, root.controller.sortOrder) }
                AppMenuItem { text: Theme.t("模型", "Models", "模型"); checkable: true; checked: root.controller.typeFilter === 2; onTriggered: root.controller.filter(search.text, 2, root.controller.sortOrder) }
                AppMenuItem { text: Theme.t("骨骼动画", "Skeletal animation", "骨骼動畫"); checkable: true; checked: root.controller.typeFilter === 3; onTriggered: root.controller.filter(search.text, 3, root.controller.sortOrder) }
                MenuSeparator {}
                AppMenuItem { text: Theme.t("名称 ↑", "Name ↑", "名稱 ↑"); onTriggered: root.controller.filter(search.text, root.controller.typeFilter, 0) }
                AppMenuItem { text: Theme.t("名称 ↓", "Name ↓", "名稱 ↓"); onTriggered: root.controller.filter(search.text, root.controller.typeFilter, 1) }
                AppMenuItem { text: Theme.t("修改时间 ↓", "Modified ↓", "修改時間 ↓"); onTriggered: root.controller.filter(search.text, root.controller.typeFilter, 2) }
                AppMenuItem { text: Theme.t("大小 ↓", "Size ↓", "大小 ↓"); onTriggered: root.controller.filter(search.text, root.controller.typeFilter, 3) }
                onClosed: filterButton.forceActiveFocus()
            }
        }
        Choice {
            objectName: "fileViewChoice"
            Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
            Layout.minimumWidth: 0
            readonly property var modes: ["list", "thumbnails", "tiles"]
            labels: [Theme.t("列表", "List", "清單"), Theme.t("缩略图", "Thumbnails", "縮圖"), Theme.t("平铺", "Tiles", "並排")]
            currentIndex: modes.indexOf(root.viewMode)
            Accessible.name: Theme.t("文件显示方式", "File view", "檔案顯示方式")
            onActivated: index => { root.remember(); root.controller.preference("fileViewMode", modes[index]) }
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.leftMargin: 6; Layout.rightMargin: 6
            Component {
                id: entryDelegate
                ItemDelegate {
                    id: entry
                    required property int index
                    required property string filePath
                    required property string fileName
                    required property string fileKind
                    required property bool isDirectory
                    required property bool selected
                    required property double fileModified
                    required property double fileBytes
                    objectName: "fileDelegate"
                    width: root.gridMode ? grid.cellWidth : list.width
                    height: root.gridMode ? grid.cellHeight : Theme.fileRowHeight
                    highlighted: selected
                    text: fileName
                    padding: 8
                    Accessible.name: fileName
                    Keys.onPressed: event => root.handleKey(event)
                    contentItem: Item {
                        Image {
                            id: thumbnail
                            objectName: "fileThumbnail"
                            anchors.top: root.thumbnails ? parent.top : undefined; anchors.left: parent.left
                            anchors.verticalCenter: root.tiles ? parent.verticalCenter : undefined
                            width: root.tiles ? 48 : parent.width; height: root.tiles ? 48 : 84; visible: root.gridMode
                            source: root.gridMode && !entry.isDirectory && entry.fileKind === "image" ? "image://thumb/"+String(Qt.btoa(Array.from(encodeURIComponent(entry.filePath), c => c.charCodeAt(0)))).replace(/\+/g,"-").replace(/\//g,"_").replace(/=/g,"")+"?"+entry.fileModified+"-"+entry.fileBytes : ""
                            sourceSize: root.tiles ? Qt.size(64,64) : Qt.size(160,120); asynchronous: true; cache: false; fillMode: Image.PreserveAspectFit
                            AppIcon { anchors.centerIn: parent; visible: parent.status !== Image.Ready; name: thumbnail.status === Image.Error ? "warning" : thumbnail.status === Image.Loading ? "loading" : entry.isDirectory ? "folder" : entry.fileKind === "image" ? "image-file" : (entry.fileKind === "model" || entry.fileKind === "convert" || entry.fileKind === "cache") ? "model" : "file"; size: 24; variant: thumbnail.status === Image.Error ? "warning" : "muted" }
                            Label {
                                objectName: "thumbnailFailure"
                                anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter
                                visible: thumbnail.status === Image.Error
                                text: Theme.t("缩略图不可用", "Thumbnail unavailable", "縮圖無法使用")
                                color: Theme.muted; font.pixelSize: 10
                            }
                        }
                        RowLayout {
                            anchors.left: parent.left; anchors.leftMargin: root.tiles ? 60 : 0; anchors.right: parent.right; anchors.verticalCenter: root.thumbnails ? undefined : parent.verticalCenter; anchors.bottom: root.thumbnails ? parent.bottom : undefined; spacing: 12
                            AppIcon { objectName: "fileTypeIcon"; visible: !root.gridMode; name: entry.isDirectory ? "folder" : entry.fileKind === "image" ? "image-file" : (entry.fileKind === "model" || entry.fileKind === "convert" || entry.fileKind === "cache") ? "model" : "file"; size: 20; variant: "muted" }
                            ColumnLayout { Layout.fillWidth: true; spacing: 4
                                Label { Layout.fillWidth: true; text: entry.fileName; color: Theme.text; font.pixelSize: 13; elide: Text.ElideMiddle }
                                Label { Layout.fillWidth: true; visible: !root.thumbnails; text: entry.isDirectory ? Theme.t("文件夹", "Folder", "資料夾") : entry.fileName.split('.').pop().toUpperCase(); font.pixelSize: 11; color: Theme.muted; elide: Text.ElideRight }
                                Label { Layout.fillWidth: true; visible: root.tiles && !entry.isDirectory; text: root.sizeText(entry.fileBytes); font.pixelSize: 11; color: Theme.muted; elide: Text.ElideRight }
                            }
                        }
                    }
                    background: Rectangle { radius: Theme.radius; color: entry.selected ? Theme.selection : entry.hovered ? Theme.raised : "transparent"; border.color: entry.activeFocus ? Theme.accent : "transparent" }
                    MouseArea {
                        anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => { if(mouse.button === Qt.RightButton) { context.targetPath = entry.filePath; context.popup() } else { root.controller.select(entry.index, mouse.modifiers); root.activeView.forceActiveFocus() } }
                        onDoubleClicked: mouse => { if(mouse.button === Qt.LeftButton) root.controller.activate(entry.index) }
                    }
                }
            }
            ListView {
                id: list
                objectName: "fileList"
                anchors.fill: parent; visible: !root.gridMode; clip: true
                model: root.visible && visible && !root.modelResetting ? root.controller.files : null; delegate: entryDelegate; reuseItems: true; cacheBuffer: Theme.fileRowHeight * 2
                focus: true; activeFocusOnTab: true
                keyNavigationEnabled: false
                ScrollBar.vertical: ScrollBar {}
                Keys.onPressed: event => root.handleKey(event)
                onMovementEnded: root.remember()
                onContentHeightChanged: if (!root.gridMode) root.restoreGeometryChanged()
                onHeightChanged: if (!root.gridMode) root.restoreGeometryChanged()
                onWidthChanged: if (!root.gridMode) root.restoreGeometryChanged()
            }
            GridView {
                id: grid
                objectName: "fileGrid"
                anchors.fill: parent; visible: root.gridMode; clip: true
                model: root.visible && visible && !root.modelResetting ? root.controller.files : null
                cellWidth: width / root.columns; cellHeight: root.tiles ? 80 : 116
                delegate: entryDelegate; cacheBuffer: 116; activeFocusOnTab: visible
                keyNavigationEnabled: false
                ScrollBar.vertical: ScrollBar {}
                Keys.onPressed: event => root.handleKey(event)
                onMovementEnded: root.remember()
                onContentHeightChanged: if (root.gridMode) root.restoreGeometryChanged()
                onHeightChanged: if (root.gridMode) root.restoreGeometryChanged()
                onWidthChanged: if (root.gridMode) root.restoreGeometryChanged()
            }
            Label { anchors.centerIn: parent; visible: root.controller.count === 0; color: Theme.muted; text: root.controller.busy ? Theme.t("正在扫描…", "Scanning…", "正在掃描…") : Theme.t("没有文件", "No files", "沒有檔案") }
        }
        Label { Layout.margins: 10; color: Theme.muted; font.pixelSize: 12; text: root.controller.count + Theme.t(" 项 · 已选 ", " items · Selected ", " 項 · 已選 ") + root.controller.selectedCount }
    }
    AppPopupMenu {
        id: context
        property string targetPath: ""
        AppMenuItem { text: Theme.t("打开", "Open", "開啟"); onTriggered: root.controller.openPath(context.targetPath) }
        AppMenuItem { text: Theme.t("复制路径", "Copy path", "複製路徑"); onTriggered: root.controller.copy(context.targetPath) }
        AppMenuItem { text: Theme.t("打开所在位置", "Show location", "開啟所在位置"); onTriggered: root.controller.reveal(context.targetPath) }
    }
    function handleKey(event) {
        if (event.modifiers & Qt.AltModifier) return
        if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) controller.selectAll()
        else if (gridMode && event.key === Qt.Key_Right) step(1,event.modifiers)
        else if (gridMode && event.key === Qt.Key_Left) step(-1,event.modifiers)
        else if (event.key === Qt.Key_Down) step(columns,event.modifiers)
        else if (event.key === Qt.Key_Up) step(-columns,event.modifiers)
        else if (event.key === Qt.Key_Home) step(-controller.count,event.modifiers)
        else if (event.key === Qt.Key_End) step(controller.count,event.modifiers)
        else if (event.key === Qt.Key_PageDown) step(10,event.modifiers)
        else if (event.key === Qt.Key_PageUp) step(-10,event.modifiers)
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) controller.activate(controller.currentIndex)
        else return
        event.accepted = true
    }
}
