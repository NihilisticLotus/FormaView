pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// M07 skeleton comparison overlay: structure and applicability verdicts are
// reported separately; the merged row list marks matched / missing /
// extra bones with a differences-only filter. Compare runs in the
// background; stale generations are rejected by the session.
Rectangle {
    id: root
    objectName: "skeletonComparePane"
    required property var session
    property bool onlyDifferences: false
    signal closed()
    color: Theme.panel
    radius: Theme.groupRadius
    border.width: 1
    border.color: Theme.line

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: Theme.t("骨架层级对比", "Skeleton comparison", "骨架層級對比")
                color: Theme.text
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Tool {
                objectName: "compareClose"
                text: Theme.t("关闭", "Close", "關閉")
                onClicked: root.closed()
                Accessible.name: Theme.t("关闭骨架对比", "Close the comparison", "關閉骨架對比")
            }
        }

        ExplainedText {
            objectName: "compareErrorMessage"
            Layout.fillWidth: true
            kind: "error"
            raw: root.session.compareError
            tone: Theme.text
        }
        Label {
            objectName: "compareStructureVerdict"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.noteText
            text: {
                const r = root.session.compareResult
                if (!r || r.structureVerdict === undefined) return ""
                const verdicts = {
                    structuralDirect: Theme.t("结构：层级一致（映射唯一，变换语义兼容需另行验证）", "Structure: hierarchy matches (unique mapping; transform semantics verified separately)", "結構：層級一致（映射唯一，變換語意相容需另行驗證）"),
                    partial: Theme.t("结构：部分匹配", "Structure: partial match", "結構：部分匹配"),
                    ambiguous: Theme.t("结构：必要映射歧义", "Structure: ambiguous required mapping", "結構：必要映射歧義"),
                    incompatible: Theme.t("结构：不兼容", "Structure: incompatible", "結構：不相容")
                }
                return verdicts[r.structureVerdict] || r.structureVerdict
            }
        }
        Label {
            objectName: "compareApplicabilityVerdict"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.noteText
            text: {
                const r = root.session.compareResult
                if (!r || r.applicabilityVerdict === undefined) return ""
                const labels = {
                    directApplicable: Theme.t("应用：可直接应用（映射唯一，双方变换数据完整）", "Applicability: directly applicable (unique mapping; complete transform evidence)", "應用：可直接應用（映射唯一，雙方變換資料完整）"),
                    compatibleNotDrivable: Theme.t("应用：变换兼容（当前格式或继承模式尚不支持直接驱动）", "Applicability: transforms compatible (direct drive is not supported for this format or inheritance mode)", "應用：變換相容（目前格式或繼承模式尚不支援直接驅動）"),
                    structureOnly: Theme.t("应用：仅结构可映射（变换数据不足，应用语义未验证）", "Applicability: structure mappable only (insufficient transform evidence)", "應用：僅結構可映射（變換資料不足，應用語意未驗證）"),
                    trialOnly: Theme.t("应用：仅可试播（部分映射；未映射骨骼保留静态姿态）", "Applicability: trial only (partial mapping; unmapped bones keep the static pose)", "應用：僅可試播（部分映射；未映射骨骼保留靜止姿態）"),
                    ambiguousMapping: Theme.t("应用：映射歧义，不可直接应用", "Applicability: ambiguous mapping; not directly applicable", "應用：映射歧義，不可直接應用"),
                    notApplicable: Theme.t("应用：不兼容", "Applicability: incompatible", "應用：不相容")
                }
                return labels[r.applicabilityVerdict] || r.applicabilityVerdict
            }
        }
        Label {
            objectName: "compareSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.muted
            font.pixelSize: Theme.noteText
            text: {
                const r = root.session.compareResult
                if (!r || r.matched === undefined) return ""
                return Theme.t("外部", "External", "外部") + " " + r.bonesA + " · " +
                       Theme.t("当前", "Current", "目前") + " " + r.bonesB + " · " +
                       Theme.t("匹配", "matched", "匹配") + " " + r.matched + " · " +
                       Theme.t("仅外部", "external-only", "僅外部") + " " + r.externalOnly + " · " +
                       Theme.t("缺失", "missing", "缺失") + " " + r.missingInExternal
            }
        }
        CheckBox {
            objectName: "compareOnlyDiff"
            text: Theme.t("仅看差异", "Differences only", "僅看差異")
            checked: root.onlyDifferences
            onToggled: root.onlyDifferences = checked
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            ListView {
                objectName: "compareRows"
                id: rows
                clip: true
                model: {
                    const r = root.session.compareResult
                    const all = (r && r.rows !== undefined) ? r.rows : []
                    if (!root.onlyDifferences) return all
                    return all.filter(row => row.status !== "matched")
                }
                delegate: Label {
                    id: row
                    required property var modelData
                    width: rows.width
                    leftPadding: 8
                    elide: Text.ElideRight
                    text: row.modelData.name + "  ·  " + ({
                        ambiguous: Theme.t("映射歧义", "ambiguous mapping", "映射歧義"),
                        parentConflict: Theme.t("父链冲突", "parent-chain conflict", "父鏈衝突"),
                        matched: Theme.t("匹配", "matched", "匹配"),
                        missingInExternal: Theme.t("外部缺失", "missing externally", "外部缺失"),
                        targetOnly: Theme.t("仅当前", "current only", "僅目前"),
                        externalOnly: Theme.t("仅外部", "external only", "僅外部")
                    })[row.modelData.status]
                    color: row.modelData.status === "matched" ? Theme.muted
                          : row.modelData.status === "missingInExternal" ? Theme.accent
                          : Theme.text
                    font.pixelSize: Theme.noteText
                }
            }
        }
    }
}
