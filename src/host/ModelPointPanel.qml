pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var session
    spacing: 0
    SectionHeader {
        id: toggle
        objectName: "modelPointToggle"
        Layout.fillWidth: true
        text: Theme.t("源点检查 · 原始 UV / 全部源影响", "Source point · raw UV / all influences", "來源點檢查 · 原始 UV / 全部來源影響")
    }
    ColumnLayout {
        objectName: "modelPointDetails"
        Layout.fillWidth: true; Layout.margins: 10
        visible: toggle.checked
        Label {
            Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap
            text: !root.session.inspectable ? Theme.t("视图准备中，精确检查暂缓。", "View preparation is pending; exact inspection is paused.", "視圖準備中，精確檢查暫緩。") : root.session.picked.point === undefined ? Theme.t("单击三维表面或 UV 选取源点；UV 重叠时显示首个三角形及重叠数量。", "Click a surface or UV triangle to inspect a source point. Overlapping UVs report the first triangle and overlap count.", "按一下三維表面或 UV 選取來源點；UV 重疊時顯示首個三角形及重疊數量。") : Theme.t("源点 %1 · 源面 %2 · 面角 %3", "Point %1 · source face %2 · face corner %3", "來源點 %1 · 來源面 %2 · 面角 %3").arg(root.session.picked.point).arg(root.session.picked.face).arg(root.session.picked.faceCorner)
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.session.inspectable && root.session.picked.point !== undefined
            Readout { Layout.fillWidth: true; label: Theme.t("源坐标", "Source position", "來源座標"); value: [root.session.picked.x,root.session.picked.y,root.session.picked.z].join(", ") }
            Readout { Layout.fillWidth: true; label: root.session.picked.uvSet || "UV"; value: root.session.picked.u === undefined ? "—" : root.session.picked.u+", "+root.session.picked.v }
            Readout { Layout.fillWidth: true; label: Theme.t("原始记录 / 和", "Raw records / sum", "原始記錄 / 和"); value: root.session.bindingIndex<0 ? "—" : (root.session.picked.count ?? 0)+" / "+(root.session.picked.sum ?? 0) }
        }
        ListView {
            objectName: "modelAllInfluences"
            Layout.fillWidth: true; Layout.preferredHeight: Math.min(contentHeight,150)
            visible: root.session.inspectable && root.session.picked.point !== undefined
            model: root.session.influences; clip: true; reuseItems: true
            delegate: RowLayout {
                id: record
                required property var modelData
                required property int index
                objectName: "modelInfluenceRow" + index
                width: ListView.view.width; height: Theme.rowHeight
                Label { Layout.fillWidth: true; text: record.modelData.bone+" ["+record.modelData.index+"]"; color: Theme.text; elide: Text.ElideMiddle; textFormat: Text.PlainText }
                Label { text: record.modelData.raw; color: Theme.text; font.pixelSize: Theme.compactText }
            }
            ScrollBar.vertical: ScrollBar {}
        }
        Label {
            Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap; font.pixelSize: Theme.noteText
            text: root.session.bindingIndex<0 ? Theme.t("当前绑定未选定或不存在；不能把未确定的数据当作零权重。", "The binding is undecided or absent; this is not zero weight.", "目前綁定未選定或不存在；不能把未確定資料視為零權重。") : root.session.picked.point !== undefined && root.session.influences.length===0 ? Theme.t("此源点没有权重记录。", "This source point has no weight records.", "此來源點沒有權重記錄。") : Theme.t("完整保留源记录，不截断、不改写。预览求值与后续导出策略相互独立。", "All source records are retained without truncation or modification. Preview evaluation and later export policy are independent.", "完整保留來源記錄，不截斷、不改寫。預覽求值與後續匯出策略相互獨立。")
        }
    }
}
