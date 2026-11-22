pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

Item {
    id: root
    required property var controller
    property bool sidebarVisible: true
    property string collection: "directory"
    property string selectedPath: ""
    property bool selectedFolder: false
    property string selectedKind: ""
    property double selectedBytes: -1
    function thumbnailUrl(path: string, stamp: string): string {
        const encoded = String(Qt.btoa(Array.from(encodeURIComponent(path), c => c.charCodeAt(0))))
        return "image://thumb/" + encoded.replace(/\+/g,"-").replace(/\//g,"_").replace(/=/g,"") + "?" + stamp
    }
    function selectCard(card): void {
        selectedPath = card.path
        selectedFolder = card.folder
        selectedKind = card.kind
        selectedBytes = directoryView ? card.model.fileBytes : -1
    }
    onFilteredPathsChanged: {
        if (!directoryView && selectedPath && !filteredPaths.includes(selectedPath)) selectedPath = ""
    }
    property string search: ""
    property bool listMode: false
    signal inspect(string path)
    signal chooseDirectory()
    readonly property bool directoryView: collection === "directory"
    readonly property string caption: directoryView ? Theme.t("目录", "Folder", "目錄") : collection === "added" ? Theme.t("已添加素材", "Added assets", "已加入素材") : Theme.t("收藏", "Favorites", "收藏")
    readonly property var paths: collection === "added" ? controller.addedAssets : controller.favoriteAssets
    readonly property var filteredPaths: paths.filter(path => root.basename(path).toLowerCase().includes(root.search.toLowerCase()))
    function basename(path: string): string { return path.replace(/\\/g, "/").split("/").pop() || path }
    function choose(key: string): void { collection = key; selectedPath = ""; selectedFolder = false }
    function addressFocus(): void { choose("directory"); Qt.callLater(() => location.addressFocus()) }
    function openSelection(): void {
        if (!selectedPath) return
        if (selectedFolder) controller.navigate(selectedPath)
        else inspect(selectedPath)
    }
    Connections {
        target: root.controller.files
        function onModelReset() { if (root.directoryView) root.selectedPath = "" }
    }
    FrostFileDialog {
        parentWindow: root.Window.window
        id: addDialog
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            root.controller.addLibraryFiles(selectedFiles)
            root.choose("added")
        }
    }
    component Surface: GlassPanel {}
    RowLayout {
        anchors.fill: parent; anchors.margins: 10; spacing: 10
        Surface {
            objectName: "libraryFileSidebar"; visible: root.sidebarVisible
            Layout.preferredWidth: root.width < 900 ? 156 : 205; Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 12; spacing: 6
                Label { text: Theme.t("文件","Files","檔案"); color: Theme.text; font.bold: true; font.pixelSize: 16; Layout.bottomMargin: 10 }
                Repeater {
                    model: ["directory","added","favorites"]
                    Tool {
                        required property string modelData
                        objectName: "collection-" + modelData
                        Layout.fillWidth: true; quiet: !checked; checked: root.collection === modelData
                        iconName: modelData === "directory" ? "folder" : modelData === "added" ? "assets" : "save"
                        text: modelData === "directory" ? Theme.t("当前目录","Current folder","目前目錄") : modelData === "added" ? Theme.t("已添加素材","Added assets","已加入素材") : Theme.t("收藏","Favorites","收藏")
                        onClicked: root.choose(modelData)
                    }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line; Layout.topMargin: 8; Layout.bottomMargin: 8 }
                LibraryFileGroups {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    controller: root.controller; collection: root.collection; search: root.search; selectedPath: root.selectedPath
                    onNavigateFolder: path => { root.choose("directory"); root.controller.navigate(path) }
                    onInspect: path => root.inspect(path)
                }
                Tool { Layout.fillWidth: true; iconName: "folder"; text: Theme.t("打开目录…","Open folder…","開啟目錄…"); onClicked: root.chooseDirectory() }
                Tool { objectName: "libraryAddFiles"; Layout.fillWidth: true; iconName: "file"; text: Theme.t("添加素材…","Add files…","加入素材…"); onClicked: addDialog.present() }
            }
        }
        Surface {
            Layout.fillWidth: true; Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; spacing: 10
                FolderNavigation { id: location; visible: root.directoryView; Layout.fillWidth: true; controller: root.controller }
                RowLayout {
                    Layout.fillWidth: true
                    Label { Layout.fillWidth: true; color: Theme.text; text: root.caption; font.bold: true; font.pixelSize: 18 }
                    Field {
                        id: searchBox; Layout.preferredWidth: Math.min(220, root.width * 0.21)
                        text: root.directoryView ? root.controller.query : root.search
                        placeholderText: Theme.t("搜索素材","Search assets","搜尋素材")
                        onTextEdited: { if (root.directoryView) root.controller.filter(text,root.controller.typeFilter,root.controller.sortOrder); else root.search=text }
                    }
                    Tool { iconName: "assets"; quiet: true; text: root.listMode ? Theme.t("网格","Grid","網格") : Theme.t("列表","List","清單"); onClicked: root.listMode = !root.listMode }
                }
                Label {
                    visible: !!root.controller.error; text: root.controller.error; color: Theme.error
                    Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                }
                GridView {
                    id: grid; objectName: "libraryGrid"; Layout.fillWidth: true; Layout.fillHeight: true
                    clip: true; activeFocusOnTab: true
                    cellWidth: root.listMode ? width : width / Math.max(1,Math.floor(width / 162))
                    cellHeight: root.listMode ? 48 : 154
                    model: !root.visible ? null : root.directoryView ? root.controller.files : root.filteredPaths
                    delegate: ItemDelegate {
                        id: card
                        // Qt supplies named roles for FileList and modelData for path collections.
                        required property var model
                        required property int index
                        readonly property string path: root.directoryView ? String(model.filePath || "") : String(root.filteredPaths[index] || "")
                        readonly property bool folder: root.directoryView && !!model.isDirectory
                        readonly property string name: root.basename(path)
                        readonly property string extension: folder ? "" : (name.includes(".") ? name.split(".").pop().toUpperCase() : "")
                        readonly property string kind: root.directoryView ? (model.fileKind || "") :
                            /^(PNG|JPE?G|BMP|TGA|WEBP|GIF|DDS|EXR|HDR|TIFF?|ICO)$/.test(extension) ? "image" :
                            /^(MP4|MKV|WEBM|MOV|AVI|M4V|TS|M2TS|MPG|MPEG|FLV)$/.test(extension) ? "video" :
                            /^(GLB|GLTF|OBJ|FBX|PSKX?|VRM|ABC)$/.test(extension) ? "model" : ""
                        readonly property bool imageFile: kind === "image"
                        objectName: "libraryCard"; width: grid.cellWidth-6; height: grid.cellHeight-6
                        Accessible.name: name; text: name; padding: 8
                        onClicked: { grid.currentIndex=index; root.selectCard(card) }
                        onDoubleClicked: { root.selectCard(card); root.openSelection() }
                        Keys.onReturnPressed: { root.selectCard(card); root.openSelection() }
                        background: Rectangle { radius: 11; color: card.path === root.selectedPath ? Theme.selection : card.hovered ? Theme.hover : "transparent"; border.color: card.visualFocus ? Theme.accent : "transparent" }
                        contentItem: ColumnLayout {
                            spacing: 6
                            Rectangle {
                                visible: !root.listMode; Layout.fillWidth: true; Layout.fillHeight: true; radius: 9; clip: true; color: Theme.thumbnail
                                AppIcon { anchors.centerIn: parent; visible: thumb.status !== Image.Ready; size: 27; name: card.folder ? "folder" : card.imageFile ? "image-file" : card.kind === "model" || card.kind === "cache" ? "model" : card.kind === "video" ? "animation-play" : "file"; variant: "muted" }
                                Image { id: thumb; anchors.fill: parent; fillMode: Image.PreserveAspectFit; asynchronous: true; sourceSize: Qt.size(240,180)
                                    objectName: "libraryThumbnail"
                                    source: !root.listMode && card.imageFile ? root.thumbnailUrl(card.path, (card.model.fileModified || 0) + "-" + (card.model.fileBytes || 0)) : ""

                                }
                            }
                            Label { visible: !root.listMode && !!card.extension; text: card.extension; color: Theme.muted; font.pixelSize: 10 }
                            Label { Layout.fillWidth: true; text: card.name; color: Theme.text; elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter }
                        }
                    }
                    Keys.onReturnPressed: { if(currentItem) {root.selectCard(currentItem); root.openSelection()} }
                    ScrollBar.vertical: ScrollBar {}
                    ColumnLayout {
                        visible: grid.count === 0
                        anchors.centerIn: parent
                        spacing: 12
                        readonly property bool filtering: (root.directoryView ? root.controller.query : root.search).length > 0
                        AppIcon { Layout.alignment: Qt.AlignHCenter; name: root.directoryView ? "folder" : "assets"; size: 28 }
                        Label {
                            Layout.alignment: Qt.AlignHCenter; color: Theme.text; font.pixelSize: 16
                            text: root.directoryView && root.controller.busy ? Theme.t("正在读取…","Loading…","正在讀取…")
                                : parent.filtering ? Theme.t("暂无匹配素材","No matching assets","暫無符合素材")
                                : root.directoryView ? Theme.t("当前目录没有可浏览的素材","No assets in this folder","目前目錄沒有可瀏覽的素材")
                                : root.collection === "added" ? Theme.t("尚未添加素材","No assets added yet","尚未加入素材") : Theme.t("尚无收藏","No favorites yet","尚無收藏")
                        }
                        Tool {
                            visible: !parent.filtering && !(root.directoryView && root.controller.busy)
                            Layout.alignment: Qt.AlignHCenter
                            text: root.collection === "added" ? Theme.t("添加素材…","Add files…","加入素材…") : Theme.t("选择目录…","Choose folder…","選擇目錄…")
                            onClicked: if (root.collection === "added") addDialog.present(); else root.chooseDirectory()
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { Layout.fillWidth: true; text: grid.count + Theme.t(" 个项目"," items"," 個項目"); color: Theme.muted }
                    Tool { objectName: "libraryFavorite"; enabled: !!root.selectedPath && !root.selectedFolder; checked: root.controller.favoriteAssets.includes(root.selectedPath); text: checked ? Theme.t("取消收藏","Unfavorite","取消收藏") : Theme.t("收藏","Favorite","收藏"); onClicked: root.controller.toggleFavorite(root.selectedPath) }
                    Tool { visible: root.collection === "added"; enabled: !!root.selectedPath; text: Theme.t("移出列表","Remove","移出清單"); onClicked: { root.controller.removeLibraryFile(root.selectedPath); root.selectedPath="" } }
                    Tool { objectName: "libraryInspect"; primary: true; enabled: !!root.selectedPath; text: root.selectedFolder ? Theme.t("打开目录","Open folder","開啟目錄") : Theme.t("打开检查","Inspect","開啟檢查"); onClicked: root.openSelection() }
                }
            }
        }
        Surface {
            visible: root.width >= 1080; Layout.preferredWidth: 238; Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 18; spacing: 14
                Label { text: Theme.t("检查器","Inspector","檢查器"); font.bold: true; font.pixelSize: 16; color: Theme.text }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
                Rectangle {
                    visible: !!root.selectedPath && root.selectedKind === "image"
                    Layout.fillWidth: true; implicitHeight: 138; radius: 10; color: Theme.thumbnail; clip: true
                    Image { anchors.fill: parent; asynchronous: true; fillMode: Image.PreserveAspectFit; sourceSize: Qt.size(320,220)
                        source: parent.visible ? root.thumbnailUrl(root.selectedPath, String(root.selectedBytes)) : "" }
                }
                Label { visible: !!root.selectedPath && root.selectedBytes >= 0 && !root.selectedFolder
                    text: Qt.locale().formattedDataSize(root.selectedBytes); color: Theme.muted }
                Label { text: root.selectedPath ? root.basename(root.selectedPath) : root.caption; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.text; font.bold: true }
                Label { text: root.selectedPath || (root.directoryView ? root.controller.directory : ""); Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.muted }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
