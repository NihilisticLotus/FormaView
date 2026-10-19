pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var source
    spacing: 6
    Readout { Layout.fillWidth: true; label: Theme.t("节点 ID", "Node ID", "節點 ID"); value: root.source.id || "—" }
    Readout { Layout.fillWidth: true; label: Theme.t("稳定来源 ID", "Stable source ID", "穩定來源 ID"); value: root.source.stableId || "—" }
    Readout { Layout.fillWidth: true; label: Theme.t("源父节点", "Source parent", "來源父節點"); value: root.source.parent || "—" }
    Readout { Layout.fillWidth: true; visible: !!root.source.rawTranslation; label: Theme.t("原始平移", "Raw translation", "原始平移"); value: (root.source.rawTranslation || []).join(", ") }
    Readout { Layout.fillWidth: true; visible: !!root.source.rawRotation; label: Theme.t("原始四元数 XYZW", "Raw quaternion XYZW", "原始四元數 XYZW"); value: (root.source.rawRotation || []).join(", ") }
    ModelMatrix { Layout.fillWidth: true; title: Theme.t("默认局部矩阵", "Default local matrix", "預設局部矩陣"); values: root.source.local || [] }
    ModelMatrix { Layout.fillWidth: true; title: Theme.t("默认世界矩阵 · 派生", "Default world matrix · derived", "預設世界矩陣 · 派生"); values: root.source.world || [] }
    ModelMatrix { Layout.fillWidth: true; title: Theme.t("几何世界矩阵 · 派生", "Geometry world matrix · derived", "幾何世界矩陣 · 派生"); values: root.source.geometryWorld || [] }
}
