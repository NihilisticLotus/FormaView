pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
ColumnLayout {
    id: root
    required property var session
    spacing: 10
                    Readout { Layout.fillWidth: true; label: Theme.t("绑定 ID", "Binding ID", "綁定 ID"); value: root.session.bindingInfo.id || "—" }
                    Readout { Layout.fillWidth: true; label: Theme.t("源方法", "Source method", "來源方法"); value: root.session.bindingInfo.method || "—" }
                    Readout { Layout.fillWidth: true; label: Theme.t("调色板节点", "Palette node", "調色盤節點"); value: root.session.bindingInfo.nodeId || "—" }
                    ModelMatrix { Layout.fillWidth: true; title: Theme.t("几何到骨骼 · 源绑定", "Geometry to bone · source binding", "幾何至骨骼 · 來源綁定"); values: root.session.bindingInfo.geometryToBone || [] }
                    ModelMatrix { Layout.fillWidth: true; title: Theme.t("绑定世界矩阵", "Bind world matrix", "綁定世界矩陣"); values: root.session.bindingInfo.bindWorld || [] }
                    ModelMatrix { Layout.fillWidth: true; title: root.session.bindingInfo.inverseBindProvided ? Theme.t("源逆绑定矩阵", "Source inverse bind matrix", "來源逆綁定矩陣") : Theme.t("逆绑定 · 缺省单位矩阵", "Inverse bind · identity default", "逆綁定 · 預設單位矩陣"); values: root.session.bindingInfo.inverseBind || [] }
                    ModelMatrix { Layout.fillWidth: true; title: Theme.t("默认骨骼世界矩阵 · 派生", "Default joint world · derived", "預設骨骼世界矩陣 · 派生"); values: root.session.bindingInfo.defaultJointWorld || [] }
}
