pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
    objectName: "conversionResults"
    required property var controller
    required property var snapshot
    property alias acknowledged: acknowledge.checked
    readonly property string approvalDigest: snapshot.approval_digest || ""
    readonly property string planId: snapshot.id || ""
    onPlanIdChanged: filters.reset()
    onApprovalDigestChanged: acknowledge.checked = false
    readonly property bool hasLoss: (snapshot.items || []).some(i => (i.preflight?.losses || []).length > 0)
    signal submit()
    signal back()
    signal cancelled()
    spacing: 10
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.accent; wrapMode: Text.WordWrap; text: root.controller.imageJobs.busy ? Theme.t("正在固定输入与准备预检…", "Preparing fixed inputs and preflight…", "正在固定輸入與準備預檢…") : Theme.state(root.snapshot.state || "") + " · " + (root.snapshot.total || 0) }
    Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WrapAnywhere; text: root.snapshot.id ? Theme.t("源数据与设置检查完成后即可转换；烘焙精度在转换时校验，失败不会写出文件。", "Source and settings checks precede conversion. Bake accuracy is checked during conversion; failed outputs are not published.", "來源資料與設定檢查完成後即可轉換；烘焙精度在轉換時驗證，失敗不會寫出檔案。") : Theme.t("修改参数后需重新预检。", "Changed parameters require a new preflight.", "修改參數後需重新預檢。") }
    PreflightFilters { id: filters; Layout.fillWidth: true; items: root.snapshot.items || []; onFilterChanged: results.positionViewAtBeginning() }
    RowLayout {
        Layout.fillWidth: true; spacing: 24
        Label { Layout.fillWidth: true; Layout.preferredWidth: 30; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("来源 / 选择", "Source / Selection", "來源 / 選擇") }
        Label { Layout.fillWidth: true; Layout.preferredWidth: 32; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("输出 / 规则", "Output / Rules", "輸出 / 規則") }
        Label { Layout.fillWidth: true; Layout.preferredWidth: 38; color: Theme.muted; font.pixelSize: Theme.compactText; text: Theme.t("结果", "Result", "結果") }
        Item { Layout.preferredWidth: 70 }
    }
    ListView {
        id: results; objectName: "preflightResultsList"
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; reuseItems: true; spacing: 6
        model: filters.filteredItems
        ScrollBar.vertical: ScrollBar {}
        delegate: ConversionItemRow {
            required property var modelData
            width: ListView.view.width; controller: root.controller; entry: modelData; preflight: true
        }
        Label { anchors.centerIn: parent; width: parent.width - 40; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; color: Theme.muted; visible: results.count === 0; text: filters.active ? Theme.t("没有符合筛选条件的结果。可调整条件或清空筛选。", "No results match these filters. Adjust or clear the filters.", "沒有符合篩選條件的結果。可調整條件或清空篩選。") : Theme.t("暂无预检结果。", "No preflight results yet.", "尚無預檢結果。") }
    }
    OptionCheck { id: acknowledge; objectName: "acknowledgeImageLosses"; visible: root.hasLoss; enabled: root.snapshot.state === "ready"; text: Theme.t("已阅读并接受本计划列出的全部损失", "I reviewed and accept all losses listed for this plan", "已閱讀並接受本計畫列出的全部損失") }
    RowLayout {
        Tool { visible: root.controller.imageJobs.busy && !root.snapshot.id; text: Theme.t("取消准备", "Cancel preparation", "取消準備"); onClicked: { root.cancelled(); root.controller.imageJobs.cancelPreparation() } }
        Tool { objectName: "editConversionSettings"; text: Theme.t("返回修改", "Edit settings", "返回修改"); onClicked: root.back() }
        Tool { visible: root.snapshot.state === "planning"; enabled: !root.snapshot.cancellation_requested; text: root.snapshot.cancellation_requested ? Theme.t("取消中", "Cancelling", "取消中") : Theme.t("取消预检", "Cancel preflight", "取消預檢"); onClicked: { root.cancelled(); root.controller.imageJobs.cancel("plan", root.snapshot.id) } }
        Tool { quiet: true; enabled: !!root.snapshot.id; text: Theme.t("复制计划快照", "Copy plan snapshot", "複製計畫快照"); onClicked: root.controller.copy(JSON.stringify(root.snapshot, null, 2)) }
        Item { Layout.fillWidth: true }
        Tool { primary: true; objectName: "submitImagePlan"; text: Theme.t("开始转换", "Start conversion", "開始轉換"); enabled: root.snapshot.state === "ready" && !root.controller.imageJobs.busy && (!root.hasLoss || acknowledge.checked); onClicked: root.submit() }
    }
}
