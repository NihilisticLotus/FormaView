pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var model
    required property string currentPath
    required property string currentDuration
    signal openVideo(string path)
    signal collapse()
    objectName: "videoPlaylist"
    color: Theme.sidebar
    clip: true

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
            model: root.model
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                id: entry
                objectName: "videoPlaylistEntry"
                required property string fileName
                required property string filePath
                required property real fileModified
                required property real fileBytes
                width: items.width
                height: VideoPreferences.playlistThumbnails ? 44 : 32
                horizontalPadding: 8
                verticalPadding: 2
                highlighted: filePath === root.currentPath
                Accessible.name: fileName
                Accessible.selectable: true
                Accessible.selected: highlighted
                ToolTip.visible: hovered && title.truncated
                ToolTip.text: fileName
                ToolTip.delay: 600
                onClicked: root.openVideo(filePath)
                background: Rectangle {
                    color: entry.highlighted ? Theme.selection : entry.hovered ? Theme.raised : "transparent"
                    border.color: entry.visualFocus ? Theme.accent : "transparent"
                    Rectangle {
                        visible: entry.highlighted
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
                        visible: entry.highlighted && !!root.currentDuration
                        text: root.currentDuration
                        color: Theme.muted
                        font.family: "Consolas"
                    }
                }
            }
        }
    }
}
