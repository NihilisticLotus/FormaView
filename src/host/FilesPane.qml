pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
Rectangle {
    id: root
    required property var controller
    property bool thumbnails: !!controller.preferences.thumbnails
    signal collapse()
    objectName: "filesPane"
    color: Theme.panel; radius: Theme.groupRadius; border.color: Theme.line
    signal editAddress()
    function addressFocus() { editAddress() }
    function viewFocus() { (thumbnails ? grid : list).forceActiveFocus() }
    Keys.onEscapePressed: event => { root.remember(); root.collapse(); event.accepted = true }
    function remember() { if (!visible) return; const view = thumbnails ? grid : list; const columns = thumbnails ? Math.max(1,Math.floor(grid.width/128)) : 1; controller.saveView(Math.max(0,view.contentY-view.originY)/(thumbnails ? grid.cellHeight : Theme.fileRowHeight)*columns) }
    property bool restorePending: false
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
        if (!visible) return
        const view = thumbnails ? grid : list
        view.forceLayout()
        const columns = thumbnails ? Math.max(1,Math.floor(grid.width/128)) : 1
        const offset = restorePosition/columns*(thumbnails ? grid.cellHeight : Theme.fileRowHeight)
        view.contentY = Math.max(view.originY, Math.min(view.originY+offset, view.originY+view.contentHeight-view.height))
    }
    function restore() {
        restorePosition = controller.scrollPosition
        restorePending = true
        Qt.callLater(applyRestore)
    }
    Connections {
        target: root.Window.window
        enabled: root.visible && root.restorePending
        function onFrameSwapped() {
            root.applyRestore()
            root.restorePending = false
        }
    }
    function step(delta, modifiers) { if (!controller.count) return; const next = Math.max(0, Math.min(controller.count-1, controller.currentIndex+delta)); if (next === controller.currentIndex) return; controller.select(next, modifiers); (thumbnails ? grid : list).positionViewAtIndex(next, ListView.Contain) }
    Connections { target: root.controller; function onViewRestored() { root.restore() } }
    onThumbnailsChanged: restore()
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
        RowLayout {
            Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10
            Tool { iconName: "list-view"; text: Theme.t("列表", "List", "清單"); checked: !root.thumbnails; onClicked: { root.remember(); root.controller.preference("thumbnails", false) } }
            Tool { iconName: "thumbnail-view"; text: Theme.t("缩略图", "Thumbnails", "縮圖"); checked: root.thumbnails; onClicked: { root.remember(); root.controller.preference("thumbnails", true) } }
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
                    width: root.thumbnails ? grid.cellWidth : list.width
                    height: root.thumbnails ? grid.cellHeight : Theme.fileRowHeight
                    highlighted: selected
                    text: fileName
                    padding: 8
                    Accessible.name: fileName
                    Keys.onPressed: event => root.handleKey(event)
                    contentItem: Item {
                        Image {
                            id: thumbnail
                            objectName: "fileThumbnail"
                            anchors.top: parent.top; width: parent.width; height: 84; visible: root.thumbnails
                            source: root.thumbnails && !entry.isDirectory && entry.fileKind === "image" ? "image://thumb/"+String(Qt.btoa(Array.from(encodeURIComponent(entry.filePath), c => c.charCodeAt(0)))).replace(/\+/g,"-").replace(/\//g,"_").replace(/=/g,"")+"?"+entry.fileModified+"-"+entry.fileBytes : ""
                            sourceSize: Qt.size(160,120); asynchronous: true; cache: false; fillMode: Image.PreserveAspectFit
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
                            anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: root.thumbnails ? undefined : parent.verticalCenter; anchors.bottom: root.thumbnails ? parent.bottom : undefined; spacing: 12
                            AppIcon { visible: !root.thumbnails; name: entry.isDirectory ? "folder" : entry.fileKind === "image" ? "image-file" : (entry.fileKind === "model" || entry.fileKind === "convert" || entry.fileKind === "cache") ? "model" : "file"; size: 20; variant: "muted" }
                            ColumnLayout { Layout.fillWidth: true; spacing: 4
                                Label { Layout.fillWidth: true; text: entry.fileName; color: Theme.text; font.pixelSize: 13; elide: Text.ElideMiddle }
                                Label { Layout.fillWidth: true; visible: !root.thumbnails; text: entry.isDirectory ? Theme.t("文件夹", "Folder", "資料夾") : entry.fileName.split('.').pop().toUpperCase(); font.pixelSize: 11; color: Theme.muted; elide: Text.ElideRight }
                            }
                        }
                    }
                    background: Rectangle { radius: Theme.radius; color: entry.selected ? Theme.selection : entry.hovered ? Theme.raised : "transparent"; border.color: entry.activeFocus ? Theme.accent : "transparent" }
                    MouseArea {
                        anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => { if(mouse.button === Qt.RightButton) { context.targetPath = entry.filePath; context.popup() } else { root.controller.select(entry.index, mouse.modifiers); (root.thumbnails ? grid : list).forceActiveFocus() } }
                        onDoubleClicked: mouse => { if(mouse.button === Qt.LeftButton) root.controller.activate(entry.index) }
                    }
                }
            }
            ListView {
                id: list
                objectName: "fileList"
                anchors.fill: parent; visible: !root.thumbnails; clip: true
                model: root.visible && !root.thumbnails && !root.modelResetting ? root.controller.files : null; delegate: entryDelegate; reuseItems: true; cacheBuffer: Theme.fileRowHeight * 2
                focus: true; activeFocusOnTab: true
                keyNavigationEnabled: false
                ScrollBar.vertical: ScrollBar {}
                Keys.onPressed: event => root.handleKey(event)
                onMovementEnded: root.remember()
            }
            GridView {
                id: grid
                objectName: "fileGrid"
                anchors.fill: parent; visible: root.thumbnails; clip: true
                model: root.visible && visible && !root.modelResetting ? root.controller.files : null
                cellWidth: width / Math.max(1, Math.floor(width/128)); cellHeight: 116
                delegate: entryDelegate; cacheBuffer: 116; activeFocusOnTab: visible
                keyNavigationEnabled: false
                ScrollBar.vertical: ScrollBar {}
                Keys.onPressed: event => root.handleKey(event)
                onMovementEnded: root.remember()
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
        else if (thumbnails && event.key === Qt.Key_Right && !(event.modifiers & Qt.AltModifier)) step(1,event.modifiers)
        else if (thumbnails && event.key === Qt.Key_Left && !(event.modifiers & Qt.AltModifier)) step(-1,event.modifiers)
        else if (event.key === Qt.Key_Down) step(thumbnails ? Math.max(1,Math.floor(grid.width/128)) : 1,event.modifiers)
        else if (event.key === Qt.Key_Up) step(thumbnails ? -Math.max(1,Math.floor(grid.width/128)) : -1,event.modifiers)
        else if (event.key === Qt.Key_Home) step(-controller.count,event.modifiers)
        else if (event.key === Qt.Key_End) step(controller.count,event.modifiers)
        else if (event.key === Qt.Key_PageDown) step(10,event.modifiers)
        else if (event.key === Qt.Key_PageUp) step(-10,event.modifiers)
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) controller.activate(controller.currentIndex)
        else return
        event.accepted = true
    }
}
