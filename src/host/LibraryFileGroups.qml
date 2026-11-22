pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var controller
    required property string collection
    required property string search
    required property string selectedPath
    signal navigateFolder(string path)
    signal inspect(string path)
    property var groups: ({})
    readonly property var folders: [...new Set([...(groups.folder || []), ...controller.recent])]
    function refresh(): void {
        if (visible) groups = controller.libraryFileGroups(collection, search)
    }
    onVisibleChanged: if (visible) Qt.callLater(refresh)
    onCollectionChanged: Qt.callLater(refresh)
    onSearchChanged: Qt.callLater(refresh)
    Component.onCompleted: Qt.callLater(refresh)
    Connections { target: root.controller.files; function onModelReset() { Qt.callLater(root.refresh) } }
    Connections { target: root.controller; function onLibraryChanged() { Qt.callLater(root.refresh) } }
    spacing: 2

    component PathList: ListView {
        id: paths
        required property bool directories
        required property string icon
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: count > 0 ? 30 : 0
        Layout.preferredHeight: Math.min(count, 4) * 30
        Layout.maximumHeight: count * 30
        clip: true
        reuseItems: true
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            id: row
            required property string modelData
            width: paths.width; height: 30; leftPadding: 18; rightPadding: 8
            text: row.modelData.split("/").pop() || row.modelData
            Accessible.name: text
            contentItem: RowLayout {
                spacing: 6
                AppIcon { name: paths.icon; size: 14 }
                Label { Layout.fillWidth: true; text: row.text; elide: Text.ElideMiddle; color: Theme.text; font.pixelSize: Theme.compactText }
            }
            background: Rectangle { radius: Theme.radius; color: row.visualFocus || row.hovered ? Theme.hover : root.selectedPath === row.modelData ? Theme.selection : "transparent" }
            ToolTip.visible: hovered; ToolTip.text: modelData
            onClicked: if (paths.directories) root.navigateFolder(modelData); else root.inspect(modelData)
        }
    }
    SectionHeader {
        id: folderHeader; objectName: "libraryFoldersHeader"; Layout.fillWidth: true; checked: true
        text: Theme.t("目录","Folders","目錄")
    }
    PathList {
        visible: folderHeader.checked && count > 0; model: root.folders; directories: true; icon: "folder"
    }
    SectionHeader {
        id: assetHeader; objectName: "libraryAssetsHeader"; Layout.fillWidth: true; checked: true
        text: Theme.t("素材列表","Asset list","素材列表")
    }
    Repeater {
        model: ["model","image","video","other"]
        ColumnLayout {
            id: group
            required property string modelData
            readonly property var paths: root.groups[modelData] || []
            visible: assetHeader.checked && paths.length > 0
            Layout.fillWidth: true
            Layout.fillHeight: groupHeader.checked
            Layout.maximumHeight: groupHeader.implicitHeight + (groupHeader.checked ? paths.length * 30 + spacing : 0)
            spacing: 1
            SectionHeader {
                id: groupHeader; objectName: "libraryGroup-" + group.modelData
                Layout.fillWidth: true; leftPadding: 17; checked: true
                text: (group.modelData === "model" ? Theme.t("模型与动画","Models and animation","模型與動畫")
                    : group.modelData === "image" ? Theme.t("图片","Images","圖片")
                    : group.modelData === "video" ? Theme.t("视频","Videos","影片") : Theme.t("其他","Other","其他")) + "  " + group.paths.length
            }
            PathList {
                visible: groupHeader.checked; model: group.paths; directories: false
                icon: group.modelData === "model" ? "model" : group.modelData === "image" ? "image-file" : group.modelData === "video" ? "animation-play" : "file"
            }
        }
    }
    Item { Layout.fillHeight: true; Layout.preferredHeight: 0 }
}
