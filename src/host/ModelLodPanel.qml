pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var session
    signal focusMesh()
    readonly property var summary: session.lodSummary
    readonly property var selected: session.classification
    readonly property bool hiddenMesh: !!selected.hiddenByLevel || !!selected.hiddenByCollision
    readonly property var levels: session.lodLevels
    readonly property real firstTriangles: levels.length ? levels[0].triangles : 0
    implicitHeight: content.implicitHeight
    ColumnLayout {
    id: content
    width: root.width; spacing: 8
    Label { text: Theme.t("LOD 与碰撞", "LOD and collision", "LOD 與碰撞"); color: Theme.text; font.bold: true }
    Readout {
        Layout.fillWidth: true; inlineValue: true
        label: Theme.t("当前显示", "Visible meshes", "目前顯示")
        value: root.summary.surface === undefined ? "—" : root.session.info.visibleMeshes + " / " + root.session.info.meshCount
    }
    Readout {
        Layout.fillWidth: true; inlineValue: true
        label: Theme.t("表面三角形", "Surface triangles", "表面三角形")
        value: root.summary.surface === undefined ? "—" : Number(root.summary.surface).toLocaleString(Qt.locale(), "f", 0)
    }
    Readout {
        Layout.fillWidth: true; inlineValue: true
        label: Theme.t("碰撞三角形", "Collision triangles", "碰撞三角形")
        value: root.summary.collision === undefined ? "—" : Number(root.summary.collision).toLocaleString(Qt.locale(), "f", 0)
    }
    Choice {
        objectName: "modelLodGroupChoice"
        Layout.fillWidth: true
        model: root.session.lodGroups; currentIndex: root.session.lodGroupIndex
        displayText: count ? currentText : Theme.t("没有已确认的 LOD 组", "No confirmed LOD group", "沒有已確認的 LOD 組")
        enabled: count>0; onActivated: root.session.selectLodGroup(currentIndex)
        Accessible.name: Theme.t("LOD 组", "LOD group", "LOD 組")
    }
    Repeater {
        model: root.levels
        delegate: Tool {
            id: levelRow
            required property var modelData
            objectName: "modelLodLevel"+modelData.level
            Layout.fillWidth: true
            readonly property string levelName: "LOD " + modelData.level
                + (modelData.current ? Theme.t(" · 当前", " · current", " · 目前") : modelData.requested ? Theme.t(" · 请求中", " · requested", " · 請求中") : "")
            readonly property string faceCount: Theme.t("%1 三角形", "%1 triangles", "%1 三角形").arg(Number(modelData.triangles).toLocaleString(Qt.locale(), "f", 0))
            readonly property string partCount: Theme.t("%1 个部件", "%1 parts", "%1 個部件").arg(modelData.meshes)
                + (root.firstTriangles ? Theme.t(" · 首级面数的 %1%", " · %1% of first level", " · 首級面數的 %1%").arg(Math.round(modelData.triangles * 100 / root.firstTriangles)) : "")
            readonly property string collisionCount: modelData.collisionMeshes ? Theme.t("碰撞部件 %1，%2 三角形", "%1 collision parts, %2 triangles", "碰撞部件 %1，%2 三角形")
                .arg(modelData.collisionMeshes).arg(modelData.collisionTriangles) : ""
            text: levelName + " · " + faceCount
            Accessible.description: partCount + " " + collisionCount
            implicitHeight: implicitContentHeight + topPadding + bottomPadding
            topPadding: 6; bottomPadding: 6
            contentItem: ColumnLayout {
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    Label { Layout.fillWidth: true; text: levelRow.levelName; font.bold: true; font.pixelSize: Theme.compactText; color: Theme.text }
                    Label { text: levelRow.faceCount; font.pixelSize: Theme.noteText; color: Theme.text }
                }
                Label { Layout.fillWidth: true; text: levelRow.partCount; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.Wrap }
                Label { Layout.fillWidth: true; visible: !!text; text: levelRow.collisionCount; font.pixelSize: Theme.noteText; color: Theme.muted; wrapMode: Text.Wrap }
            }
            checked: modelData.current
            onClicked: root.session.requestLod(modelData.level)
        }
    }
    Choice {
        objectName: "modelCollisionChoice"
        Layout.fillWidth: true
        model: [Theme.t("隐藏碰撞", "Hide collision", "隱藏碰撞"),Theme.t("叠加碰撞", "Overlay collision", "疊加碰撞"),Theme.t("仅碰撞", "Collision only", "僅碰撞")]
        currentIndex: root.session.collisionMode; onActivated: root.session.setCollisionMode(currentIndex)
        Accessible.name: Theme.t("碰撞显示", "Collision display", "碰撞顯示")
    }
    Label { text: Theme.t("网格归类", "Mesh classification", "網格歸類"); font.bold: true; color: Theme.text }
    Choice {
        objectName: "modelLodMesh"; Layout.fillWidth: true
        model: root.session.meshes; currentIndex: root.session.meshIndex; enabled: count > 0
        Accessible.name: Theme.t("归类网格", "Mesh to classify", "歸類網格")
        onActivated: index => root.session.selectMesh(index)
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted
        visible: root.hiddenMesh
        text: [root.selected.hiddenByLevel ? Theme.t("当前显示的是其他 LOD 等级。", "A different LOD is displayed.", "目前顯示的是其他 LOD 等級。") : "",
               root.selected.hiddenByCollision ? Theme.t("当前碰撞筛选隐藏了此网格。", "The collision filter hides this mesh.", "目前碰撞篩選隱藏了此網格。") : ""].filter(Boolean).join(" ")
    }
    Tool {
        objectName: "modelLodMeshAction"; Layout.fillWidth: true
        enabled: root.session.inspectable && root.session.meshIndex >= 0 && root.session.meshes.length > 0
        text: root.hiddenMesh ? Theme.t("显示此网格", "Show this mesh", "顯示此網格") : Theme.t("聚焦此网格", "Frame this mesh", "聚焦此網格")
        onClicked: { if (root.hiddenMesh) root.session.revealSelectedMesh(); else root.focusMesh() }
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.muted; textFormat: Text.PlainText
        text: root.session.classification.evidence || Theme.t("源文件未提供已识别的显式 LOD 关联。", "No recognized explicit LOD association in this source.", "來源檔案未提供已識別的顯式 LOD 關聯。")
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.muted; textFormat: Text.PlainText
        visible: !!root.session.classification.candidate
        text: root.session.classification.candidate || ""
    }
    Tool { objectName: "modelClassifyMesh"; Layout.fillWidth: true; text: Theme.t("会话显示归类…", "Session display classification…", "工作階段顯示歸類…"); onClicked: correction.open() }
    Tool { Layout.fillWidth: true; text: Theme.t("恢复源归类", "Restore source classification", "還原來源歸類"); enabled: !!root.session.classification.corrected; onClicked: root.session.restoreClassification() }
    Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; font.pixelSize: Theme.noteText
        text: Theme.t("归类仅影响本次显示，不写回资产，也不推断未导出的物理资产。准备或失败时，所有查看页的精确检查持续暂停。", "Classification only affects this session. It never writes assets or infers unexported physics assets. Exact inspection remains paused across modes while preparation is pending or failed.", "歸類僅影響本次顯示，不寫回資產，亦不推斷未匯出的物理資產。準備或失敗時，所有檢視頁的精確檢查持續暫緩。")
    }
    }
    FrostDialog {
        id: correction
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(420, parent ? parent.width-32 : 420)
        modal: true; focus: true; title: Theme.t("本次显示归类", "Classification for this session", "本次顯示歸類")
        font.pixelSize: Theme.formText; padding: 24
        background: Rectangle { color: Theme.raised; radius: Theme.dialogRadius; border.color: Theme.line }
        footer: RowLayout {
            spacing: 12
            Item { Layout.fillWidth: true }
            Tool { form: true; text: Theme.t("取消", "Cancel", "取消"); onClicked: correction.reject() }
            Tool { form: true; primary: true; text: Theme.t("确定", "OK", "確定"); onClicked: correction.accept() }
        }
        onOpened: { groupName.text=root.session.classification.group || ""; level.value=root.session.classification.level || 0; collision.checked=!!root.session.classification.collision }
        onAccepted: root.session.classifyMesh(groupName.text,level.value,collision.checked)
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: Theme.t("组名留空表示普通网格。相同组名和等级可包含多个部件。", "Leave the group empty for ordinary geometry. A group and level may contain multiple parts.", "組名留空表示一般網格。相同組名及等級可包含多個部件。"); color: Theme.muted }
            Field { id: groupName; objectName: "modelClassificationGroup"; form: true; Layout.fillWidth: true; maximumLength: 120; placeholderText: Theme.t("LOD 组名", "LOD group name", "LOD 組名") }
            RowLayout {
                Label { text: "LOD"; color: Theme.text }
                NumberField { id: level; form: true; from: 0; to: 999; editable: true }
            }
            OptionCheck { id: collision; font.pixelSize: Theme.formText; text: Theme.t("此网格作为碰撞几何显示", "Display this mesh as collision geometry", "此網格作為碰撞幾何顯示") }
        }
    }
}
