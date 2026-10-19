pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQml.Models

ColumnLayout {
    id: root
    required property var session
    readonly property var selected: session.skeletonSelection
    readonly property string selectionKey: (selected.id || "") + ":" + (selected.boneIndex ?? -1)
    onSelectionKeyChanged: revealTimer.restart()
    spacing: 8
    Timer {
        id: revealTimer
        interval: 0
        onTriggered: {
            if (search.text || root.session.skeleton.weightedOnly) tree.expandRecursively()
            tree.reveal(false)
        }
    }
    Timer { id: positionTimer; interval: 0; onTriggered: tree.positionSelection() }
    Label { text: Theme.t("骨骼层级 · 只读", "Skeleton hierarchy · Read only", "骨骼階層 · 唯讀"); color: Theme.text; font.bold: true }
    Label {
        Layout.fillWidth: true
        visible: root.session.bindings.length===1
        text: root.session.bindings[0] || ""
        color: Theme.muted; elide: Text.ElideMiddle; textFormat: Text.PlainText
    }
    Choice {
        objectName: "modelSkinBindingChoice"
        Layout.fillWidth: true
        visible: root.session.bindings.length>1
        model: root.session.bindings; currentIndex: root.session.bindingIndex
        displayText: currentIndex<0 ? Theme.t("请选择绑定", "Choose a binding", "請選取綁定") : currentText
        onActivated: root.session.selectBinding(currentIndex)
        Accessible.name: Theme.t("蒙皮绑定", "Skin binding", "蒙皮綁定")
    }
    Field {
        id: search
        objectName: "modelBoneSearch"
        Layout.fillWidth: true
        placeholderText: Theme.t("搜索骨骼名称", "Search bone names", "搜尋骨骼名稱")
        onTextEdited: root.session.skeleton.searchText=text
        Accessible.name: placeholderText
    }
    RowLayout {
        Layout.fillWidth: true
        Choice {
            objectName: "modelBoneFilter"
            Layout.fillWidth: true
            model: [Theme.t("全部骨骼", "All bones", "全部骨骼"), Theme.t("仅有权重", "Weighted only", "僅有權重")]
            currentIndex: root.session.skeleton.weightedOnly ? 1 : 0
            onActivated: root.session.skeleton.weightedOnly=currentIndex===1
            Accessible.name: Theme.t("骨骼筛选", "Bone filter", "骨骼篩選")
        }
        Tool { objectName: "modelBoneLocate"; iconName: "focus"; hint: Theme.t("定位选中骨骼", "Locate selected bone", "定位選取骨骼"); enabled: !!root.selected.id; onClicked: tree.reveal(true) }
        Tool { objectName: "modelBoneCollapse"; iconName: "collapse"; hint: Theme.t("全部折叠", "Collapse all", "全部摺疊"); onClicked: tree.collapseRecursively() }
    }
    TreeView {
        id: tree
        objectName: "modelBoneTree"
        Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 100
        model: root.session.skeleton
        clip: true; editTriggers: TableView.NoEditTriggers
        selectionModel: ItemSelectionModel {
            model: root.session.skeleton
            onCurrentChanged: (current, previous) => { if (current.valid) root.session.selectSkeletonIndex(current) }
        }
        columnWidthProvider: () => tree.width
        function reveal(clearFilters: bool): void {
            if (clearFilters) {
                search.text=""
                root.session.skeleton.searchText=""
                root.session.skeleton.weightedOnly=false
            }
            const target=root.session.skeleton.indexForNode(root.selected.id || "", root.selected.boneIndex ?? -1)
            if (target.valid) {
                tree.expandToIndex(target)
                positionTimer.restart()
            }
        }
        function positionSelection(): void {
            const current=root.session.skeleton.indexForNode(root.selected.id || "", root.selected.boneIndex ?? -1)
            if (current.valid) tree.positionViewAtIndex(current, TableView.Contain)
        }
        Component.onCompleted: revealTimer.restart()
        onHeightChanged: revealTimer.restart()
        Connections { target: root.session; function onDocumentChanged() { revealTimer.restart() } }
        Connections { target: root.session.skeleton; function onFilterChanged() { revealTimer.restart() } }
        delegate: TreeViewDelegate {
            id: bone
            required property int column
            required property string jointLabel
            required property string nodeKey
            required property int jointIndex
            readonly property bool chosen: root.selected.id===nodeKey && root.selected.boneIndex===jointIndex
            implicitWidth: tree.width; implicitHeight: Theme.rowHeight
            indentation: 12
            text: jointLabel
            contentItem: RowLayout {
                spacing: 7
                AppIcon { name: "weights"; variant: bone.chosen ? "normal" : "muted"; Layout.preferredWidth: 16; Layout.preferredHeight: 16 }
                Label { Layout.fillWidth: true; text: bone.jointLabel; color: bone.chosen ? Theme.text : Theme.muted; elide: Text.ElideMiddle; textFormat: Text.PlainText; verticalAlignment: Text.AlignVCenter; font.pixelSize: Theme.compactText }
            }
            background: Rectangle {
                color: bone.chosen ? Theme.selection : bone.hovered ? Theme.hover : "transparent"
                radius: Theme.radius
                border.color: bone.activeFocus ? Theme.accent : "transparent"
                Rectangle { width: 3; height: parent.height-8; anchors.verticalCenter: parent.verticalCenter; color: Theme.accent; visible: bone.chosen; radius: 1 }
            }
            onClicked: root.session.selectSkeletonIndex(tree.index(row,column))
            Accessible.name: jointLabel
        }
        ScrollBar.vertical: ScrollBar {}
        Label {
            anchors.centerIn: parent; width: parent.width; horizontalAlignment: Text.AlignHCenter
            visible: tree.rows===0; wrapMode: Text.Wrap; color: Theme.muted
            text: root.session.bindingIndex<0 ? Theme.t("当前网格没有已选蒙皮绑定", "No skin binding selected for this mesh", "目前網格沒有已選蒙皮綁定") : Theme.t("没有匹配的骨骼", "No matching bones", "沒有符合的骨骼")
        }
    }
    Label { Layout.fillWidth: true; text: root.selected.label || "—"; textFormat: Text.PlainText; elide: Text.ElideMiddle; color: Theme.text; font.bold: true }
    Readout { Layout.fillWidth: true; label: Theme.t("父级", "Parent", "父層"); value: root.selected.parentName || "—" }
    Readout { Layout.fillWidth: true; label: Theme.t("影响源点", "Affected source points", "影響來源點"); value: root.selected.inPalette ? String(root.selected.points ?? 0) : "—" }
    Tool {
        objectName: "modelBoneHeat"
        Layout.fillWidth: true
        text: Theme.t("查看权重热力", "Show weight heat map", "檢視權重熱力")
        enabled: !!root.selected.inPalette
        onClicked: { root.session.selectBone(root.selected.boneIndex); root.session.setMode(2) }
    }
    SectionHeader { id: advanced; objectName: "modelBindingMatrices"; Layout.fillWidth: true; text: Theme.t("高级详情", "Advanced details", "進階詳情") }
    ScrollView {
        id: details
        Layout.fillWidth: true; Layout.preferredHeight: 180
        visible: advanced.checked
        contentWidth: availableWidth
        ColumnLayout {
            width: details.availableWidth
            Loader { objectName: "modelBindingDetailsLoader"; Layout.fillWidth: true; active: advanced.checked && !!root.selected.inPalette; visible: active; sourceComponent: Component { ModelBindingDetails { session: root.session } } }
            Loader { objectName: "modelSourceDetailsLoader"; Layout.fillWidth: true; active: advanced.checked; visible: active; sourceComponent: Component { ModelSourceDetails { source: root.selected } } }
        }
    }
}
