pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var session
    implicitHeight: content.implicitHeight
    ColumnLayout {
    id: content
    width: root.width; spacing: 8
    Choice {
        Layout.fillWidth: true
        model: root.session.lodGroups; currentIndex: root.session.lodGroupIndex
        displayText: count ? currentText : Theme.t("没有已确认的 LOD 组", "No confirmed LOD group", "沒有已確認的 LOD 組")
        enabled: count>0; onActivated: root.session.selectLodGroup(currentIndex)
        Accessible.name: Theme.t("LOD 组", "LOD group", "LOD 組")
    }
    Repeater {
        model: root.session.lodLevels
        delegate: Tool {
            required property var modelData
            Layout.fillWidth: true
            text: "LOD "+modelData.level+" · "+modelData.triangles+Theme.t(" 三角形", " triangles", " 三角形")+(modelData.current ? Theme.t(" · 当前", " · current", " · 目前") : "")+(modelData.requested&&!modelData.current ? Theme.t(" · 请求中", " · requested", " · 請求中") : "")
            checked: modelData.current
            onClicked: root.session.requestLod(modelData.level)
        }
    }
    Choice {
        Layout.fillWidth: true
        model: [Theme.t("隐藏碰撞", "Hide collision", "隱藏碰撞"),Theme.t("叠加碰撞", "Overlay collision", "疊加碰撞"),Theme.t("仅碰撞", "Collision only", "僅碰撞")]
        currentIndex: root.session.collisionMode; onActivated: root.session.setCollisionMode(currentIndex)
        Accessible.name: Theme.t("碰撞显示", "Collision display", "碰撞顯示")
    }
    Readout { Layout.fillWidth: true; label: Theme.t("当前网格", "This mesh", "目前網格"); value: root.session.meshes[root.session.meshIndex] || "—" }
    Label {
        Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.muted; textFormat: Text.PlainText
        text: root.session.classification.evidence || Theme.t("源文件未提供已识别的显式 LOD 关联。", "No recognized explicit LOD association in this source.", "來源檔案未提供已識別的顯式 LOD 關聯。")
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.muted; textFormat: Text.PlainText
        visible: !!root.session.classification.candidate
        text: root.session.classification.candidate || ""
    }
    Tool { Layout.fillWidth: true; text: Theme.t("会话显示归类…", "Session display classification…", "工作階段顯示歸類…"); onClicked: correction.open() }
    Tool { Layout.fillWidth: true; text: Theme.t("恢复源归类", "Restore source classification", "還原來源歸類"); enabled: !!root.session.classification.corrected; onClicked: root.session.restoreClassification() }
    Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; font.pixelSize: Theme.noteText
        text: Theme.t("归类仅影响本次显示，不写回资产，也不推断未导出的物理资产。准备或失败时，所有查看页的精确检查持续暂停。", "Classification only affects this session. It never writes assets or infers unexported physics assets. Exact inspection remains paused across modes while preparation is pending or failed.", "歸類僅影響本次顯示，不寫回資產，亦不推斷未匯出的物理資產。準備或失敗時，所有檢視頁的精確檢查持續暫緩。")
    }
    }
    Dialog {
        id: correction
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(420, parent ? parent.width-32 : 420)
        modal: true; title: Theme.t("本次显示归类", "Classification for this session", "本次顯示歸類")
        standardButtons: Dialog.Ok|Dialog.Cancel
        onOpened: { groupName.text=root.session.classification.group || ""; level.value=root.session.classification.level || 0; collision.checked=!!root.session.classification.collision }
        onAccepted: root.session.classifyMesh(groupName.text,level.value,collision.checked)
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: Theme.t("组名留空表示普通网格。相同组名和等级可包含多个部件。", "Leave the group empty for ordinary geometry. A group and level may contain multiple parts.", "組名留空表示一般網格。相同組名及等級可包含多個部件。"); color: Theme.muted }
            Field { id: groupName; Layout.fillWidth: true; maximumLength: 120; placeholderText: Theme.t("LOD 组名", "LOD group name", "LOD 組名") }
            RowLayout {
                Label { text: "LOD"; color: Theme.text }
                SpinBox { id: level; from: 0; to: 999; editable: true }
            }
            OptionCheck { id: collision; text: Theme.t("此网格作为碰撞几何显示", "Display this mesh as collision geometry", "此網格作為碰撞幾何顯示") }
        }
    }
}
