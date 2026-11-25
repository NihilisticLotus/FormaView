pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var controller
    readonly property bool listFocused: items.activeFocus
    readonly property bool menuOpen: contextMenu.visible
    required property string currentPath
    required property string currentDuration
    signal openVideo(string path)
    signal collapse()
    objectName: "videoPlaylist"
    color: Theme.sidebar
    clip: true

    AppPopupMenu {
        id: contextMenu
        objectName: "playlistContextMenu"
        property string targetPath
        AppMenuItem {
            text: Theme.t("播放", "Play", "播放")
            onTriggered: root.openVideo(contextMenu.targetPath)
        }
        AppMenuItem {
            objectName: "playlistRevealFile"
            text: Theme.t("在文件资源管理器中显示", "Show in File Explorer", "在檔案總管中顯示")
            onTriggered: root.controller.reveal(contextMenu.targetPath)
        }
        AppMenuItem {
            text: Theme.t("复制文件路径", "Copy file path", "複製檔案路徑")
            onTriggered: root.controller.copy(contextMenu.targetPath)
        }
        MenuSeparator {}
        AppMenuItem {
            objectName: "playlistRemoveFile"
            text: Theme.t("从播放列表移除", "Remove from playlist", "從播放清單移除") + "    Del"
            onTriggered: root.controller.removePlaylistVideo(contextMenu.targetPath)
        }
    }

    function thumbnailUrl(path: string, modified: real, bytes: real): string {
        const encoded = String(Qt.btoa(Array.from(encodeURIComponent(path), c => c.charCodeAt(0))))
        return "image://thumb/video/" + encoded.replace(/\+/g, "-").replace(/\//g, "_").replace(/=/g, "") + "?" + modified + "-" + bytes
    }

    Rectangle {
        anchors.left: parent.left
        height: parent.height
        width: 1
        color: Theme.line
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 1
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("播放列表")
                color: Theme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Tool {
                objectName: "videoPlaylistViewToggle"
                quiet: true
                iconName: VideoPreferences.playlistThumbnails ? "list-view" : "thumbnail-view"
                hint: VideoPreferences.playlistThumbnails
                    ? Theme.t("切换到紧凑列表", "Show compact list", "切換到緊湊清單")
                    : Theme.t("显示视频缩略图", "Show video thumbnails", "顯示影片縮圖")
                onClicked: VideoPreferences.playlistThumbnails = !VideoPreferences.playlistThumbnails
            }
            Tool {
                objectName: "collapseVideoPlaylist"
                quiet: true
                iconName: "files-close"
                hint: qsTr("收起播放列表")
                onClicked: root.collapse()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.line
        }
        ListView {
            id: items
            objectName: "videoPlaylistItems"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.controller?.videos ?? null
            activeFocusOnTab: true
            keyNavigationEnabled: true
            Keys.onDeletePressed: event => {
                if (!event.isAutoRepeat && currentItem) root.controller.removePlaylistVideo(currentItem.filePath)
                event.accepted = true
            }
            Keys.onReturnPressed: if (currentItem) root.openVideo(currentItem.filePath)
            Keys.onEnterPressed: if (currentItem) root.openVideo(currentItem.filePath)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                id: entry
                objectName: "videoPlaylistEntry"
                required property string fileName
                required property int index
                required property string filePath
                required property real fileModified
                required property real fileBytes
                width: items.width
                height: VideoPreferences.playlistThumbnails ? 44 : 32
                horizontalPadding: 8
                verticalPadding: 2
                readonly property bool playing: filePath === root.currentPath
                highlighted: items.activeFocus || contextMenu.visible ? ListView.isCurrentItem : playing
                Accessible.name: fileName
                Accessible.selectable: true
                Accessible.selected: highlighted
                ToolTip.visible: hovered && title.truncated
                ToolTip.text: fileName
                ToolTip.delay: 600
                onClicked: {
                    items.currentIndex = index
                    items.forceActiveFocus()
                }
                onDoubleClicked: root.openVideo(filePath)
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: point => {
                        items.currentIndex = entry.index
                        items.forceActiveFocus()
                        contextMenu.targetPath = entry.filePath
                        contextMenu.popup(entry, point.position.x, point.position.y)
                    }
                }
                background: Rectangle {
                    color: entry.highlighted ? Theme.selection : entry.hovered ? Theme.raised : "transparent"
                    border.color: entry.visualFocus ? Theme.accent : "transparent"
                    Rectangle {
                        visible: entry.playing
                        width: 3
                        height: parent.height
                        color: Theme.accent
                    }
                }
                contentItem: RowLayout {
                    spacing: 6
                    Rectangle {
                        Layout.preferredWidth: VideoPreferences.playlistThumbnails ? 64 : 18
                        Layout.preferredHeight: VideoPreferences.playlistThumbnails ? 36 : 18
                        color: VideoPreferences.playlistThumbnails ? Theme.thumbnail : "transparent"
                        FileTypeIcon {
                            anchors.centerIn: parent
                            kind: "video"
                            size: VideoPreferences.playlistThumbnails ? 24 : 16
                            visible: poster.status !== Image.Ready
                        }
                        Image {
                            id: poster
                            objectName: "videoPlaylistThumbnail"
                            anchors.fill: parent
                            source: root.visible && VideoPreferences.playlistThumbnails
                                ? root.thumbnailUrl(entry.filePath, entry.fileModified, entry.fileBytes) : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false // The provider owns a bounded cache keyed by file revision.
                            Accessible.ignored: true
                        }
                    }
                    Label {
                        id: title
                        Layout.fillWidth: true
                        text: entry.fileName
                        color: Theme.text
                        elide: Text.ElideRight
                    }
                    Label {
                        visible: entry.playing && !!root.currentDuration
                        text: root.currentDuration
                        color: Theme.muted
                        font.family: "Consolas"
                    }
                }
            }
        }
    }
}
