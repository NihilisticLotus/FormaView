pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Timeline for Alembic geometry caches: sample time, playback, stepping,
// scrubbing and random jumps, with the presented sample's statistics.
// Skeleton-only controls (root lock, pairing, external drive) do not apply.
Rectangle {
    id: root
    objectName: "cacheTimeline"
    required property var session
    property bool collapsed: false
    readonly property int controlHeight: 28
    readonly property var cache: root.session.cacheInfo
    readonly property bool ready: root.session.geometryCache && root.cache.samples !== undefined
    readonly property int lastSample: root.ready ? root.cache.samples - 1 : 0
    readonly property bool playable: root.ready && root.cache.samples > 1
    function seconds(value: real): string { return Number(value).toFixed(3) + " s" }
    color: Theme.panel
    radius: Theme.radius
    implicitHeight: collapsed ? expandRow.implicitHeight + 8 : content.implicitHeight + 12

    RowLayout {
        id: expandRow
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4
        visible: root.collapsed
        Tool {
            objectName: "cacheTimelineExpand"
            text: Theme.t("时间轴", "Timeline", "時間軸")
            Layout.preferredHeight: Theme.controlHeight
            onClicked: root.collapsed = false
            Accessible.name: Theme.t("展开几何缓存时间轴", "Expand the geometry cache timeline", "展開幾何快取時間軸")
        }
        Label {
            Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.muted; font.pixelSize: Theme.noteText
            text: root.ready ? Theme.t("采样 %1 / %2", "sample %1 / %2", "取樣 %1 / %2").arg(root.cache.sample).arg(root.lastSample) + " · " + root.seconds(root.cache.time) : ""
        }
    }

    ColumnLayout {
        id: content
        anchors.fill: parent; anchors.margins: 6; spacing: 2
        visible: !root.collapsed
        Flow {
            Layout.fillWidth: true; spacing: 6
            Tool {
                objectName: "cachePlay"
                height: root.controlHeight; quiet: true
                iconName: root.session.playing ? "animation-pause" : "animation-play"
                hint: root.playable ? (root.session.playing ? Theme.t("暂停", "Pause", "暫停") : Theme.t("播放", "Play", "播放")) : Theme.t("只有一个采样，无法播放", "Only one sample; nothing to play", "只有一個取樣，無法播放")
                enabled: root.playable
                onClicked: root.session.togglePlay()
                Accessible.name: root.session.playing ? Theme.t("暂停几何缓存", "Pause geometry cache", "暫停幾何快取") : Theme.t("播放几何缓存", "Play geometry cache", "播放幾何快取")
            }
            Tool {
                objectName: "cacheStepBack"
                height: root.controlHeight; quiet: true; iconName: "animation-step-back"
                enabled: root.ready && root.cache.requested > 0
                hint: Theme.t("上一采样", "Previous sample", "上一取樣")
                onClicked: root.session.stepFrame(-1)
                Accessible.name: hint
            }
            Tool {
                objectName: "cacheStepForward"
                height: root.controlHeight; quiet: true; iconName: "animation-step-forward"
                enabled: root.ready && root.cache.requested < root.lastSample
                hint: Theme.t("下一采样", "Next sample", "下一取樣")
                onClicked: root.session.stepFrame(1)
                Accessible.name: hint
            }
            OptionCheck {
                objectName: "cacheLoop"
                height: root.controlHeight
                text: Theme.t("循环", "Loop", "循環")
                checked: root.session.loop; enabled: root.playable
                onToggled: root.session.setLoop(checked)
            }
            Row {
                height: root.controlHeight; spacing: 6
                Label { anchors.verticalCenter: parent.verticalCenter; text: Theme.t("跳到采样", "Go to sample", "跳至取樣"); color: Theme.muted; font.pixelSize: Theme.noteText }
                TextField {
                    id: sampleInput
                    objectName: "cacheSampleInput"
                    width: 72; height: root.controlHeight
                    enabled: root.ready
                    horizontalAlignment: TextInput.AlignHCenter
                    color: Theme.text
                    validator: IntValidator { bottom: 0; top: Math.max(0, root.lastSample) }
                    background: Rectangle { radius: 4; color: Theme.field; border.width: sampleInput.activeFocus ? 2 : 1; border.color: sampleInput.activeFocus ? Theme.accent : Theme.line }
                    text: root.ready ? String(root.cache.requested) : ""
                    onEditingFinished: if (acceptableInput) root.session.seekSample(parseInt(text))
                    Accessible.name: Theme.t("采样索引", "Sample index", "取樣索引")
                }
                Label { anchors.verticalCenter: parent.verticalCenter; text: "/ " + root.lastSample; color: Theme.muted; font.pixelSize: Theme.noteText }
            }
            Tool {
                objectName: "cacheCancel"
                height: root.controlHeight
                visible: root.ready && root.cache.loading === true
                text: Theme.t("取消读取", "Cancel read", "取消讀取")
                onClicked: root.session.cancelSample()
            }
            Tool {
                objectName: "cacheCollapse"
                height: root.controlHeight; quiet: true; iconName: "collapse"
                hint: Theme.t("收起时间轴", "Collapse timeline", "收合時間軸")
                onClicked: root.collapsed = true
                Accessible.name: hint
            }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Label {
                objectName: "cacheTime"
                Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight
                color: Theme.muted; font.family: "Consolas"; font.pixelSize: Theme.noteText
                text: root.ready ? Theme.t("采样 %1 / %2 · %3", "sample %1 / %2 · %3", "取樣 %1 / %2 · %3").arg(root.cache.sample).arg(root.lastSample).arg(root.seconds(root.cache.time))
                                   + (root.cache.loading ? " · " + Theme.t("正在读取采样 %1…", "reading sample %1…", "正在讀取取樣 %1…").arg(root.cache.requested) : "") : ""
            }
            Label {
                objectName: "cacheStatistics"
                Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight
                color: Theme.muted; font.pixelSize: Theme.noteText
                text: root.ready ? Theme.t("点 %1 · 面 %2 · 角 %3", "points %1 · faces %2 · corners %3", "點 %1 · 面 %2 · 角 %3").arg(root.cache.points).arg(root.cache.faces).arg(root.cache.corners) : ""
            }
        }
        Slider {
            objectName: "cacheScrubber"
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 24
            enabled: root.playable
            from: root.ready ? root.cache.start : 0
            to: root.ready && root.cache.end > root.cache.start ? root.cache.end : from + 1
            value: root.session.time
            onMoved: root.session.seek(value)
            Accessible.name: Theme.t("几何缓存时间轴拖动", "Geometry cache timeline scrub", "幾何快取時間軸拖動")
        }
        ExplainedText {
            objectName: "cacheError"
            Layout.fillWidth: true
            visible: root.ready && !!root.cache.error
            kind: "error"
            raw: root.ready ? (root.cache.error || "") : ""
        }
        Label {
            objectName: "cacheSkeletonNote"
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.tertiary; font.pixelSize: Theme.noteText
            text: Theme.t("几何缓存只播放源采样，不推断骨架、权重或根运动；骨骼相关功能在此模式不可用。", "Geometry caches play source samples only; no skeleton, weights or root motion are inferred, so skeleton features are unavailable here.", "幾何快取只播放來源取樣，不推斷骨架、權重或根運動；骨骼相關功能在此模式不可用。")
        }
    }
}
