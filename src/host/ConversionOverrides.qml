pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: root
    required property var inputs
    required property var inputInfo
    property var overrides: ({})
    readonly property int selected: source.currentIndex
    readonly property var current: overrides[selected] || ({})
    readonly property var inspected: inputInfo.find(v => v.input === inputs[selected]) || ({})
    readonly property var names: [""].concat((inspected.channels || []).map(v => v.name))
    signal edited()
    function options(): var { return inputs.map((v, index) => overrides[index] || ({})) }
    function setOption(key, value): void {
        const copy = Object.assign({}, overrides)
        const options = Object.assign({}, current)
        if (value === undefined) delete options[key]
        else options[key] = value
        copy[selected] = options
        overrides = copy
        edited()
    }
    function mapping(destination, name): void {
        const channels = (current.channels || []).filter(v => v.destination !== destination)
        if (name) channels.push({source: name, destination: destination})
        setOption("channels", channels)
        if (destination === "A") setOption("alpha_channel", name || undefined)
    }
    onInputsChanged: overrides = ({})
    Tool { id: expand; checkable: true; text: (checked ? "▾ " : "▸ ") + Theme.t("逐项覆盖 · 子资源与真实通道", "Per-item overrides · Subresource and source channels", "逐項覆寫 · 子資源與實際通道") }
    ColumnLayout {
        visible: expand.checked; Layout.fillWidth: true
        ComboBox { id: source; objectName: "overrideInput"; Layout.fillWidth: true; model: root.inputs; Accessible.name: Theme.t("覆盖哪项输入", "Input to override", "覆寫哪項輸入") }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("未覆盖的参数沿用统一设置。先预检可读取实际通道；更改子资源后需重新预检再选择该子资源的通道。", "Other parameters inherit the shared settings. Preflight reads actual channels; after changing subresource, preflight again before mapping its channels.", "未覆寫參數沿用統一設定。先預檢可讀取實際通道；變更子資源後須重新預檢再選擇其通道。") }
        RowLayout {
            Label { textFormat: Text.PlainText; color: Theme.text; text: Theme.t("子资源索引", "Subresource index", "子資源索引") }
            CheckBox { id: resourceOverride; text: Theme.t("覆盖", "Override", "覆寫"); checked: root.current.subresource !== undefined; onToggled: root.setOption("subresource", checked ? resource.value : undefined) }
            SpinBox { id: resource; objectName: "overrideSubresource"; from: 0; to: 65535; editable: true; enabled: resourceOverride.checked; value: root.current.subresource || 0; onValueModified: root.setOption("subresource", value); Accessible.name: Theme.t("逐项子资源索引", "Per-item subresource", "逐項子資源索引") }
        }
        ComboBox {
            Layout.fillWidth: true
            readonly property var scalars: ["", "uint8", "uint16", "half", "float32"]
            model: [Theme.t("位深：沿用统一设置", "Precision: inherit shared setting", "位深：沿用統一設定"), "uint8", "uint16", "half", "float32"]
            currentIndex: scalars.indexOf(root.current.output_scalar || "")
            onActivated: root.setOption("output_scalar", scalars[currentIndex] || undefined)
            Accessible.name: Theme.t("逐项位深", "Per-item precision", "逐項位深")
        }
        CheckBox {
            objectName: "overrideMapping"; enabled: root.names.length > 1
            text: Theme.t("覆盖为 RGBA 映射；其余通道不输出", "Override RGBA mapping; omit other channels", "覆寫為 RGBA 映射；其餘通道不輸出")
            checked: root.current.channels !== undefined
            onToggled: {
                if (!checked) { root.setOption("channels", undefined); root.setOption("alpha_channel", undefined); return }
                const channels = ["R", "G", "B", "A"].filter(v => root.names.includes(v)).map(v => ({source: v, destination: v}))
                root.setOption("channels", channels.length ? channels : [{source: root.names[1], destination: "R"}])
                if (root.names.includes("A")) root.setOption("alpha_channel", "A")
            }
        }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; color: Theme.muted; text: (root.inspected.channels || []).map(v => v.name + " " + v.scalar).join(" / ") || Theme.t("尚未读取此输入的通道", "Channels have not been read for this input", "尚未讀取此輸入的通道") }
        Repeater {
            model: ["R", "G", "B", "A"]
            RowLayout {
                id: channelRow
                required property string modelData
                Layout.fillWidth: true; visible: root.current.channels !== undefined
                Label { textFormat: Text.PlainText; color: Theme.text; text: channelRow.modelData }
                ComboBox {
                    Layout.fillWidth: true; model: root.names
                    currentIndex: root.names.indexOf((root.current.channels || []).find(v => v.destination === channelRow.modelData)?.source || "")
                    onActivated: root.mapping(channelRow.modelData, currentText)
                    Accessible.name: channelRow.modelData + Theme.t(" 输出映射", " output mapping", " 輸出映射")
                }
            }
        }
        Tool { text: Theme.t("清除此项覆盖", "Reset this item's overrides", "清除此項覆寫"); enabled: Object.keys(root.current).length > 0; onClicked: { const copy = Object.assign({}, root.overrides); delete copy[root.selected]; root.overrides = copy; root.edited() } }
    }
}
