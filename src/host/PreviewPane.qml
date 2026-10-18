import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Rectangle {
    id: root
    required property var controller
    signal openFile()
    signal openFolder()
    signal locate()
    signal toggleFocus()
    signal unavailable()
    property bool focused: false
    readonly property bool imageActive: !!controller.image && (controller.image.available || controller.image.busy || !!controller.image.error)
    function focusLayoutControl() { imageFocus.forceActiveFocus() }
    objectName: "previewPane"
    color: Theme.panel; radius: Theme.groupRadius; border.color: Theme.line
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 48; color: Theme.panel
            RowLayout {
                anchors.fill: parent; anchors.margins: 12
                Label { Layout.fillWidth: true; font.pixelSize: Theme.compactText; font.bold: true; elide: Text.ElideMiddle; color: Theme.text; text: root.controller.asset.name || Theme.t("预览", "Preview", "預覽") }
                Label { objectName: "readOnlyPreviewLabel"; text: Theme.t("只读预览", "Read-only preview", "唯讀預覽"); color: Theme.muted; font.pixelSize: Theme.noteText; Accessible.description: Theme.t("预览调整仅影响显示，不修改原文件。", "Preview adjustments only affect the display; source files remain unchanged.", "預覽調整僅影響顯示，不修改原始檔案。") }
                Tool { id: imageFocus; quiet: true; iconName: "fullscreen"; text: root.focused ? Theme.t("恢复布局", "Restore layout", "還原配置") : Theme.t("专注预览", "Focus view", "專注預覽"); onClicked: root.toggleFocus() }
                Tool { iconName: "more"; quiet: true; hint: Theme.t("更多", "More", "更多"); onClicked: actions.open(); AppPopupMenu { id: actions; y: parent.height
                    AppMenuItem { text: Theme.t("打开所在位置", "Show location", "開啟所在位置"); enabled: !!root.controller.asset.path; onTriggered: root.controller.reveal(root.controller.asset.path) }
                    AppMenuItem { text: Theme.t("关闭当前文件", "Close current file", "關閉目前檔案"); enabled: !!root.controller.asset.path; onTriggered: root.controller.clearAsset() }
                } }
            }
        }
        Rectangle {
            visible: root.controller.error.length > 0
            Layout.fillWidth: true; implicitHeight: errorColumn.implicitHeight+24; color: "#43302e"
            ColumnLayout {
                id: errorColumn
                anchors.fill: parent; anchors.margins: 12
                Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.error; text: Theme.t("无法打开：", "Unable to open: ", "無法開啟：")+root.controller.requested }
                Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.text; text: root.controller.error; font.pixelSize: 13 }
                Label { Layout.fillWidth: true; visible: !!root.controller.asset.path; wrapMode: Text.WrapAnywhere; color: Theme.muted; text: Theme.t("当前仍显示：", "Still showing: ", "目前仍顯示：")+(root.controller.asset.name || "") }
                RowLayout {
                    Tool { text: Theme.t("手动定位…", "Locate…", "手動定位…"); onClicked: root.locate() }
                    Tool { text: Theme.t("复制报告", "Copy report", "複製報告"); onClicked: root.controller.copy(root.controller.report()) }
                    Tool { text: Theme.t("关闭提示", "Dismiss", "關閉提示"); onClicked: root.controller.dismissError() }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.base
            visible: !root.imageActive
            ColumnLayout {
                anchors.centerIn: parent; width: Math.min(parent.width-48, 520); spacing: 16
                Label { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; text: root.controller.asset.path ? (root.controller.asset.suffix || Theme.t("文件", "File", "檔案")) : "FormaView"; color: Theme.text; font.pixelSize: 24 }
                Label { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; color: Theme.muted; font.pixelSize: 14; text: root.controller.asset.path ? Theme.t("文件读取检查完成\n专业预览将在对应阶段接入", "File access checked\nProfessional preview is not connected yet", "檔案讀取檢查完成\n專業預覽將於對應階段接入") : Theme.t("打开文件或文件夹\n无需创建项目", "Open a file or folder\nNo project required", "開啟檔案或資料夾\n無需建立專案") }
                RowLayout { Layout.alignment: Qt.AlignHCenter
                    Tool { primary: true; iconName: "file"; text: Theme.t("打开文件…", "Open file…", "開啟檔案…"); onClicked: root.openFile() }
                    Tool { iconName: "assets"; text: Theme.t("打开目录…", "Open folder…", "開啟資料夾…"); onClicked: root.openFolder() }
                }
            }
        }
        Loader {
            Layout.fillWidth: true; Layout.fillHeight: true
            active: root.imageActive; visible: active
            onActiveChanged: { if (active) setSource("ImagePane.qml", {controller: root.controller}) }
            Component.onCompleted: { if (active) setSource("ImagePane.qml", {controller: root.controller}) }
        }
    }
}
