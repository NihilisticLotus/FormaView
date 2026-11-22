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
            Layout.margins: 12
            Label {
                Layout.fillWidth: true
                text: qsTr("播放列表")
                color: Theme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
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
                required property string fileName
                required property string filePath
                width: items.width
                height: 56
                highlighted: filePath === root.currentPath
                Accessible.name: fileName
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
                    spacing: 10
                    Rectangle {
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 28
                        radius: 4
                        color: Theme.panel
                        AppIcon {
                            anchors.centerIn: parent
                            name: "animation-play"
                            size: 16
                        }
                    }
                    Label {
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
