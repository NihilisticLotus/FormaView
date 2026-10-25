pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

RowLayout {
    id: root
    required property var controller
    signal beforeNavigate()
    objectName: "folderNavigation"
    spacing: 6
    implicitHeight: 38
    Layout.preferredHeight: 38
    Layout.minimumHeight: 38
    Layout.maximumHeight: 38
    property bool editing: false
    property string menuLabel: ""
    property var menuAnchor: null
    property string lastDirectory: controller.directory
    readonly property Item popupOverlay: root.Overlay.overlay
    readonly property real popupAvailableWidth: popupOverlay ? popupOverlay.width : 0
    readonly property real popupAvailableHeight: popupOverlay ? popupOverlay.height : 0
    readonly property var crumbs: controller.breadcrumbs(controller.directory)
    function crumbWidth(crumb): real { return Math.max(64, Math.min(170, Math.ceil(metrics.advanceWidth(crumb.name)) + 44)) }
    readonly property var visibleCrumbs: {
        const all = root.crumbs
        if (!all.length) return []
        let budget = pathBox.width - 12
        let total = 0
        for (const crumb of all) total += root.crumbWidth(crumb)
        if (total <= budget || all.length <= 2) return all
        const result = [all[0]]
        budget -= root.crumbWidth(all[0]) + 32
        const tail = []
        for (let i = all.length - 1; i > 0; --i) {
            const size = root.crumbWidth(all[i])
            if (tail.length && size > budget) break
            tail.unshift(all[i]); budget -= size
        }
        return result.concat(tail)
    }
    readonly property var hiddenCrumbs: root.crumbs.filter(crumb => !root.visibleCrumbs.some(shown => shown.path === crumb.path))
    FontMetrics { id: metrics; font.pixelSize: Theme.compactText; font.family: root.controller.language === "en" ? "Segoe UI Variable Text" : "Microsoft YaHei UI" }
    function addressFocus() {
        childrenPopup.close(); ancestorsMenu.close()
        editing = true
        address.text = controller.directory
        address.forceActiveFocus(); address.selectAll()
    }
    function go(path) { beforeNavigate(); controller.navigate(path) }
    function showChildren(anchor, crumb) {
        if (!root.popupOverlay) return
        ancestorsMenu.close()
        menuAnchor = anchor; menuLabel = crumb.name
        controller.listFolders(crumb.path)
        const point = anchor.mapToItem(root.popupOverlay, 0, anchor.height + 6)
        childrenPopup.x = Math.max(8, Math.min(point.x, root.popupOverlay.width - childrenPopup.width - 8))
        childrenPopup.y = point.y
        childrenPopup.open(); childList.forceActiveFocus()
    }
    function focusCurrent() {
        const item = crumbRepeater.itemAt(crumbRepeater.count - 1)
        if (item) item.focusName()
        else editLocation.forceActiveFocus()
    }
    function hasFocus() {
        for (let item = root.Window.window.activeFocusItem; item; item = item.parent)
            if (item === root) return true
        return false
    }
    Connections {
        target: root.controller
        function onChanged() {
            if (root.lastDirectory === root.controller.directory) return
            const restoreFocus = root.hasFocus() || childrenPopup.opened
            root.lastDirectory = root.controller.directory
            childrenPopup.close(); ancestorsMenu.close()
            if (!address.activeFocus) root.editing = false
            if (restoreFocus) Qt.callLater(root.focusCurrent)
        }
        function onFolderMenuChanged() {
            if (childrenPopup.opened && !root.controller.folderMenu.busy)
                Qt.callLater(childList.selectPath)
        }
    }
    onVisibleChanged: if (!visible) { childrenPopup.close(); ancestorsMenu.close(); editing = false }
    Tool {
        objectName: "folderBack"; iconName: "return"; enabled: root.controller.canBack
        hint: Theme.t("返回 · Alt+左", "Back · Alt+Left", "返回 · Alt+左")
        onClicked: { root.beforeNavigate(); root.controller.history(-1) }
    }
    Tool {
        objectName: "folderForward"; iconName: "return"; iconRotation: 180; enabled: root.controller.canForward
        hint: Theme.t("前进 · Alt+右", "Forward · Alt+Right", "前進 · Alt+右")
        onClicked: { root.beforeNavigate(); root.controller.history(1) }
    }
    Tool {
        objectName: "folderUp"; iconName: "return"; iconRotation: 90; enabled: root.crumbs.length > 1
        hint: Theme.t("上级目录 · Alt+上", "Parent · Alt+Up", "上層目錄 · Alt+上")
        onClicked: { root.beforeNavigate(); root.controller.parentDirectory() }
    }
    Rectangle {
        id: pathBox
        Layout.fillWidth: true; Layout.minimumWidth: 110; Layout.preferredHeight: 36
        color: Theme.field; radius: Theme.radius
        border.color: root.controller.error ? Theme.error : Theme.line
        ToolTip.visible: pathHover.hovered && !!root.controller.error
        ToolTip.text: Explain.errorText(root.controller.error) + "\n" + root.controller.error
        HoverHandler { id: pathHover }
        RowLayout {
            anchors.fill: parent; anchors.margins: 2; spacing: 0
            visible: !root.editing
            Repeater {
                id: crumbRepeater
                model: root.visibleCrumbs
                RowLayout {
                    id: crumb
                    required property var modelData
                    required property int index
                    spacing: 0
                    Layout.fillWidth: index === root.visibleCrumbs.length - 1
                    Layout.minimumWidth: 50 + (index === 0 && root.hiddenCrumbs.length ? 32 : 0)
                    Layout.preferredWidth: root.crumbWidth(modelData) + (index === 0 && root.hiddenCrumbs.length ? 32 : 0)
                    Layout.maximumWidth: root.crumbWidth(modelData) + (index === 0 && root.hiddenCrumbs.length ? 32 : 0)
                    function focusName() { nameButton.forceActiveFocus() }
                    Tool {
                        id: nameButton; objectName: "breadcrumbName"; property string folderPath: crumb.modelData.path
                        Layout.fillWidth: true; Layout.minimumWidth: 18
                        quiet: true; text: crumb.modelData.name; hint: crumb.modelData.path
                        leftPadding: 6; rightPadding: 6
                        Accessible.name: Theme.t("跳转到 ", "Go to ", "跳至 ") + crumb.modelData.path
                        onClicked: root.go(crumb.modelData.path)
                    }
                    Tool {
                        objectName: "breadcrumbChildren"; property string folderPath: crumb.modelData.path
                        Layout.minimumWidth: 32; Layout.preferredWidth: 32; Layout.minimumHeight: 32; implicitWidth: 32; quiet: true; iconName: "expand"; iconRotation: 90
                        checked: childrenPopup.opened && root.menuAnchor === this
                        hint: Theme.t("展开子目录 · ", "Show subfolders · ", "展開子資料夾 · ") + crumb.modelData.name
                        onClicked: root.showChildren(this, crumb.modelData)
                    }
                    Tool {
                        id: overflow; objectName: "hiddenAncestors"; visible: crumb.index === 0 && root.hiddenCrumbs.length > 0
                        quiet: true; iconName: "more"; Layout.minimumWidth: 32; Layout.preferredWidth: 32; Layout.minimumHeight: 32
                        hint: Theme.t("其他上层目录", "More parent folders", "其他上層資料夾")
                        onClicked: ancestorsMenu.popup(overflow, 0, overflow.height)
                    }
                }
            }
            Tool {
                id: editLocation; objectName: "editLocation"
                Layout.fillWidth: true; Layout.minimumWidth: root.crumbs.length ? 12 : 80
                quiet: true; text: root.crumbs.length ? "" : Theme.t("输入目录路径", "Enter folder path", "輸入目錄路徑")
                hint: Theme.t("编辑完整路径 · Ctrl+L", "Edit full path · Ctrl+L", "編輯完整路徑 · Ctrl+L")
                onClicked: root.addressFocus()
            }
        }
        Field {
            id: address; objectName: "address"; anchors.fill: parent; visible: root.editing
            Accessible.name: Theme.t("位置", "Location", "位置")
            onAccepted: if (!inputMethodComposing) { root.go(text); root.editing = false; root.focusCurrent() }
            onActiveFocusChanged: if (!activeFocus && !inputMethodComposing) root.editing = false
            Keys.onEscapePressed: event => { root.editing = false; root.focusCurrent(); event.accepted = true }
        }
    }
    Tool {
        objectName: "copyFolderPath"; iconName: "copy"; enabled: !!root.controller.directory
        hint: Theme.t("复制完整路径", "Copy full path", "複製完整路徑")
        onClicked: root.controller.copy(root.controller.directory)
    }
    AppPopupMenu {
        id: ancestorsMenu
        Repeater {
            model: root.hiddenCrumbs
            AppMenuItem {
                required property var modelData
                text: modelData.name; Accessible.description: modelData.path
                onTriggered: root.go(modelData.path)
            }
        }
    }
    Popup {
        id: childrenPopup; objectName: "folderChildrenPopup"; parent: root.popupOverlay
        width: Math.max(0, Math.min(300, root.popupAvailableWidth - 16))
        height: Math.max(0, Math.min(370, Math.max(144, childList.contentHeight + 86), root.popupAvailableHeight - y - 8))
        padding: 8; focus: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.panel; radius: Theme.radius; border.color: Theme.line }
        onOpened: Qt.callLater(childList.selectPath)
        onClosed: {
            root.controller.closeFolderMenu()
            if (root.menuAnchor && root.menuAnchor.visible) root.menuAnchor.forceActiveFocus()
        }
        ColumnLayout {
            anchors.fill: parent; spacing: 6
            Label {
                Layout.fillWidth: true; Layout.leftMargin: 6; Layout.preferredHeight: 24
                text: root.menuLabel + Theme.t(" 下的文件夹", " subfolders", " 下的資料夾")
                elide: Text.ElideMiddle; color: Theme.muted; font.pixelSize: Theme.compactText
            }
            ListView {
                id: childList; objectName: "folderChildrenList"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; reuseItems: true
                model: root.controller.folderMenu.entries || []
                currentIndex: count ? 0 : -1
                ScrollBar.vertical: ScrollBar {}
                function selectPath() {
                    const entries = root.controller.folderMenu.entries || []
                    const current = entries.findIndex(entry => root.crumbs.some(crumb => crumb.path.toLowerCase() === entry.path.toLowerCase()))
                    currentIndex = current >= 0 ? current : count ? 0 : -1
                    if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                }
                function chooseCurrent() {
                    if (currentIndex < 0 || currentIndex >= count) return
                    const path = model[currentIndex].path
                    childrenPopup.close(); root.go(path)
                }
                Keys.onReturnPressed: chooseCurrent()
                Keys.onEnterPressed: chooseCurrent()
                delegate: ItemDelegate {
                    id: child; required property var modelData; required property int index
                    objectName: "folderChild"; property string folderPath: modelData.path
                    width: ListView.view.width; height: Theme.rowHeight
                    highlighted: ListView.isCurrentItem; hoverEnabled: true
                    text: modelData.name; Accessible.description: modelData.path
                    readonly property bool inPath: root.crumbs.some(crumb => crumb.path.toLowerCase() === child.modelData.path.toLowerCase())
                    function choose() { const path = modelData.path; childrenPopup.close(); root.go(path) }
                    onClicked: choose()
                    onHoveredChanged: if (hovered) childList.currentIndex = index
                    contentItem: RowLayout {
                        spacing: 10
                        AppIcon { name: "folder"; size: 18 }
                        Label { Layout.fillWidth: true; text: child.text; color: Theme.text; elide: Text.ElideMiddle; font.pixelSize: Theme.compactText }
                        AppIcon { visible: child.inPath; name: "confirm"; size: 16 }
                    }
                    background: Rectangle { radius: Theme.radius; color: child.highlighted ? Theme.selection : "transparent" }
                    ToolTip.visible: hovered; ToolTip.text: modelData.path; ToolTip.delay: 600
                }
                Label {
                    anchors.fill: parent; anchors.margins: 6; visible: childList.count === 0
                    text: root.controller.folderMenu.busy ? Theme.t("正在读取…", "Loading…", "正在讀取…") : root.controller.folderMenu.error ? Theme.t("无法读取子目录", "Cannot read subfolders", "無法讀取子資料夾") : Theme.t("没有子目录", "No subfolders", "沒有子資料夾")
                    color: root.controller.folderMenu.error ? Theme.error : Theme.muted
                    wrapMode: Text.Wrap; font.pixelSize: Theme.compactText
                    ToolTip.visible: errorHover.hovered && !!root.controller.folderMenu.error
                    ToolTip.text: root.controller.folderMenu.error ? Explain.errorText(root.controller.folderMenu.error) + "\n" + root.controller.folderMenu.error : ""
                    HoverHandler { id: errorHover }
                }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.line }
            Label {
                Layout.fillWidth: true; Layout.leftMargin: 6; Layout.preferredHeight: 24
                text: Theme.t("目录名跳转 · 箭头展开子目录", "Name to navigate · Arrow for subfolders", "目錄名跳轉 · 箭頭展開子資料夾")
                color: Theme.muted; font.pixelSize: Theme.noteText; elide: Text.ElideRight
            }
        }
    }
}
