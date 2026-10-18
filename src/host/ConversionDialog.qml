pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Dialog {
    id: root
    objectName: "conversionDialog"
    required property var controller
    property var inputs: []
    property int initialTab: 0
    readonly property var service: controller.imageJobs
    anchors.centerIn: Overlay.overlay
    width: Math.min(1100, Overlay.overlay.width - 40); height: Math.min(780, Overlay.overlay.height - 40)
    modal: true; focus: true
    title: Theme.t("图片转换 · 源内容", "Image conversion · Source content", "影像轉換 · 來源內容")
    footer: DialogButtonBox {
        Button { text: Theme.t("关闭", "Close", "關閉"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        onRejected: root.reject()
    }
    onOpened: { inputs = controller.selectedImages(); settings.inputRoot = controller.directory; tabs.currentIndex = initialTab }
    ColumnLayout {
        anchors.fill: parent; spacing: 10
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("默认使用源内容；视口通道、曝光、背景和平铺不进入转换参数。", "Uses source content; viewport channels, exposure, background and tiling are not conversion settings.", "預設使用來源內容；視口通道、曝光、背景和平鋪不會成為轉換參數。") }
        TabBar {
            id: tabs; Layout.fillWidth: true
            TabButton { text: Theme.t("1 设置", "1 Settings", "1 設定") }
            TabButton { text: Theme.t("2 预检", "2 Preflight", "2 預檢") }
            TabButton { text: Theme.t("3 任务", "3 Jobs", "3 工作") }
        }
        Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; visible: !!root.service.error; text: root.service.error; color: Theme.error }
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: tabs.currentIndex
            ConversionSettings {
                id: settings; controller: root.controller; inputs: root.inputs
                onChooseInputs: paths => root.inputs = paths
                onEdited: { root.service.invalidatePlan(); results.acknowledged = false }
                onPreflight: (scope, options, nameTemplate, conflict) => { root.service.preflight(root.inputs, scope, options, nameTemplate, conflict, settings.itemOverrides()); tabs.currentIndex = 1 }
            }
            ConversionResults { id: results; controller: root.controller; snapshot: root.service.plan; onSubmit: { root.service.submit(acknowledged); tabs.currentIndex = 2 } onBack: tabs.currentIndex = 0 }
            ImageJobsPane { controller: root.controller; onRetry: id => { root.service.retry(id); tabs.currentIndex = 1; results.acknowledged = false } }
        }
    }
}
