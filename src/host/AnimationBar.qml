pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Rectangle {
    id: root
    objectName: "modelAnimationBar"
    required property var session
    // Collapsed mode frees vertical preview space (focus mode friendly);
    // session-only state, no persistence.
    property bool collapsed: false
    readonly property var clipData: root.session.clipInfo
    readonly property bool ready: root.session.animated && root.clipData && root.clipData.name !== undefined
    function speedText(value: real): string {
        return value >= 0.01 && value < 1e21 && Number(value.toFixed(2)) === value
            ? value.toFixed(2) : value.toString()
    }
    color: Theme.panel
    radius: Theme.groupRadius
    border.width: 1
    border.color: Theme.line
    implicitHeight: (collapsed ? expandRow.implicitHeight + 8
                               : content.implicitHeight + 2 * 10)

    RowLayout {
        id: expandRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 4
        visible: root.collapsed
        Tool {
            objectName: "animBarExpand"
            text: Theme.t("时间轴", "Timeline", "時間軸")
            Layout.preferredHeight: Theme.controlHeight
            onClicked: root.collapsed = false
            Accessible.name: Theme.t("展开动画时间轴", "Expand the animation timeline", "展開動畫時間軸")
        }
        Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: Theme.muted
            font.pixelSize: Theme.noteText
            text: root.ready ? ("%1 s · %2 %3 / %4")
                .arg(root.session.time.toFixed(3)).arg(Theme.t("帧", "frame", "幀"))
                .arg(root.clipData.frame).arg(root.clipData.lastFrame) : ""
        }
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 10
        spacing: 4
        visible: !root.collapsed
        Flow {
            id: controls
            objectName: "animControls"
            Layout.fillWidth: true
            spacing: 6
            Tool {
                objectName: "animPlay"
                height: Theme.controlHeight
                iconName: root.session.playing ? "animation-pause" : "animation-play"
                text: root.session.playing ? Theme.t("暂停", "Pause", "暫停") : Theme.t("播放", "Play", "播放")
                enabled: root.ready
                onClicked: root.session.togglePlay()
                Accessible.name: root.session.playing ? Theme.t("暂停动画", "Pause animation", "暫停動畫") : Theme.t("播放动画", "Play animation", "播放動畫")
            }
            Tool {
                objectName: "animStepBack"; height: Theme.controlHeight; iconName: "animation-step-back"; enabled: root.ready
                hint: Theme.t("上一帧", "Previous frame", "上一幀")
                onClicked: root.session.stepFrame(-1)
                Accessible.name: Theme.t("上一帧", "Previous frame", "上一幀")
            }
            Tool {
                objectName: "animStepForward"; height: Theme.controlHeight; iconName: "animation-step-forward"; enabled: root.ready
                hint: Theme.t("下一帧", "Next frame", "下一幀")
                onClicked: root.session.stepFrame(1)
                Accessible.name: Theme.t("下一帧", "Next frame", "下一幀")
            }
            OptionCheck {
                objectName: "animLoop"; height: Theme.controlHeight
                text: Theme.t("循环", "Loop", "循環"); checked: root.session.loop; enabled: root.ready
                onToggled: root.session.setLoop(checked)
            }
            Row {
                height: Theme.controlHeight; spacing: 6
                Label { anchors.verticalCenter: parent.verticalCenter; text: Theme.t("速度", "Speed", "速度"); color: Theme.muted }
                // Manual speed entry only: plain editable field with an x
                // suffix. Accept any finite positive speed without range caps.
                TextField {
                    id: speedInput
                    objectName: "animSpeed"
                    width: 76; height: Theme.controlHeight
                    enabled: root.ready
                    horizontalAlignment: TextInput.AlignHCenter
                    color: Theme.text
                    background: Rectangle {
                        radius: 4; color: Theme.field
                        border.width: parent && parent.activeFocus ? 2 : 1
                        border.color: parent && parent.activeFocus ? Theme.accent : Theme.line
                    }
                    Binding {
                        target: speedInput; property: "text"
                        value: root.speedText(root.session.speed)
                        when: !speedInput.activeFocus
                        restoreMode: Binding.RestoreNone
                    }
                    font.family: "Consolas"
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    validator: DoubleValidator { bottom: 0; decimals: -1; notation: DoubleValidator.ScientificNotation; locale: "C" }
                    onEditingFinished: {
                        const parsed = Number(text)
                        if (isFinite(parsed) && parsed > 0)
                            root.session.setSpeed(parsed)
                        text = root.speedText(root.session.speed)
                    }
                    Accessible.name: Theme.t("播放速度（正数，回车确认）", "Playback speed (positive number, press Enter)", "播放速度（正數，按 Enter 確認）")
                }
                Label { anchors.verticalCenter: parent.verticalCenter; text: "×"; color: Theme.muted }
            }
            Row {
                height: Theme.controlHeight; spacing: 6
                Label { anchors.verticalCenter: parent.verticalCenter; text: Theme.t("片段", "Clip", "片段"); color: Theme.muted }
                Choice {
                    objectName: "animClip"; width: Math.min(150, controls.width - 70); height: Theme.controlHeight
                    model: root.session.clips; textRole: "name"; currentIndex: root.session.clipIndex; enabled: root.ready
                    onActivated: index => root.session.setClipIndex(index)
                    Accessible.name: Theme.t("动画片段", "Animation clip", "動畫片段")
                }
            }
        }
            RowLayout {
                Layout.fillWidth: true; Layout.minimumWidth: 0
                spacing: 6
                visible: root.session.externalInfo.driven === true || root.session.externalInfo.status === "refused"
                Label {
                    objectName: "animExternalStatus"
                    Layout.fillWidth: true; Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    color: root.session.externalInfo.status === "verified" ? Theme.muted : Theme.accent
                    font.pixelSize: Theme.noteText
                    text: (root.session.externalInfo.status === "refused"
                           ? Theme.t("外部动画未应用", "External animation not applied", "外部動畫未套用")
                           : root.session.externalInfo.status === "verified"
                           ? Theme.t("外部动画 · 已验证映射", "External animation · verified mapping", "外部動畫 · 已驗證映射")
                           : Theme.t("外部动画 · 试播（未验证兼容）", "External animation · trial (unverified)", "外部動畫 · 試播（未驗證相容）"))
                          + " · " + Explain.noteText(root.session.externalInfo.notice)
                    ToolTip.visible: extHover.hovered
                    ToolTip.text: Explain.noteText(root.session.externalInfo.notice) + "\n" + (root.session.externalInfo.notice || "") + "\n" + (root.session.externalInfo.path || "") + "\n" + Theme.t("关系仅存于查看会话，不改写目标或源文件。", "Session-only relationship; neither file is modified.", "關聯僅存於檢視工作階段，不改寫目標或來源檔案。")
                    HoverHandler { id: extHover }
                }
                Tool {
                    objectName: "animExternalRemove"
                    visible: root.session.externalInfo.driven === true
                    text: Theme.t("移除外部动画", "Remove external animation", "移除外部動畫")
                    enabled: !root.session.busy
                    onClicked: root.session.detachExternalAnimation()
                }
            }
        RowLayout {
            Layout.fillWidth: true
            visible: true
            spacing: 6
            Label {
                objectName: "animReferenceStatus"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                visible: root.session.info.psa === true || !!root.session.info.referencePath
                color: Theme.muted
                font.pixelSize: Theme.noteText
                text: root.session.info.referencePath
                    ? (root.session.info.companionPath
                       ? Theme.t("已使用参考骨架及轨道配置 · 仅预览骨架", "Reference skeleton and track config applied · skeleton preview", "已使用參考骨架及軌道設定 · 僅預覽骨架")
                       : Theme.t("已使用参考骨架 · 未应用轨道配置", "Reference skeleton applied · no track config", "已使用參考骨架 · 未套用軌道設定"))
                    : root.session.info.referenceRejected
                      ? Theme.t("参考骨架不适用于此 PSA · 未配对，详情见警告", "Reference skeleton does not fit this PSA · unpaired; see warnings", "參考骨架不適用於此 PSA · 未配對，詳情見警告")
                      : Theme.t("未配对 PSA · 父链未验证，姿态仅供参考", "Unpaired PSA · hierarchy unverified; pose is provisional", "未配對 PSA · 父鏈未驗證，姿態僅供參考")
                ToolTip.visible: statusHover.hovered
                ToolTip.text: text + (root.session.info.referencePath ? "\n" + root.session.info.referencePath : "") + (root.session.info.companionPath ? "\n" + root.session.info.companionPath : "") + (root.session.info.referenceRejectReason ? "\n" + root.session.info.referenceRejectReason : "")
                HoverHandler { id: statusHover }
            }
            Tool {
                objectName: "animPairMeshToggle"
                checkable: true
                checked: root.session.pairMeshVisible
                visible: !!root.session.info.referencePath
                enabled: visible && !root.session.busy
                text: Theme.t("配对模型", "Paired model", "配對模型")
                onToggled: root.session.setPairMeshVisible(checked)
                Accessible.name: Theme.t("显示配对模型而非骨架", "Show the paired model instead of the skeleton", "顯示配對模型而非骨架")
            }
            Row {
                height: Theme.controlHeight; spacing: 6
                CheckBox {
                    objectName: "animRootLock"
                    text: Theme.t("强制根锁定", "Force Root Lock", "強制根鎖定")
                    checked: root.session.rootLockOn
                    enabled: root.ready && (root.session.rootLockOn || root.session.rootLockInfo.rootMapped !== false)
                    onToggled: root.session.setRootLock(checked)
                    Accessible.name: Theme.t("锁定根节点平移与旋转，保留缩放；仅预览，不改源数据", "Lock root translation/rotation, keep scale; preview only", "鎖定根節點平移與旋轉，保留縮放；僅預覽")
                }
                Choice {
                    objectName: "animRootBase"
                    width: 108; height: Theme.controlHeight
                    model: [Theme.t("动画首帧", "Anim first frame", "動畫首幀"), Theme.t("参考姿态", "Reference pose", "參考姿態"), Theme.t("零点", "Zero", "零點")]
                    currentIndex: root.session.rootBase
                    enabled: root.ready
                    onActivated: index => root.session.setRootBase(index)
                    Accessible.name: Theme.t("根锁定基准", "Root lock base", "根鎖定基準")
                }
                Choice {
                    objectName: "animRootBone"
                    width: 120; height: Theme.controlHeight
                    visible: root.session.rootCandidates.length > 1
                    enabled: root.ready
                    model: root.session.rootCandidates
                    textRole: "name"
                    currentIndex: {
                        for (var i = 0; i < root.session.rootCandidates.length; ++i)
                            if (root.session.rootCandidates[i].index === root.session.rootLockInfo.root)
                                return i
                        return -1
                    }
                    onActivated: index => root.session.setRootIndex(root.session.rootCandidates[index].index)
                    Accessible.name: Theme.t("选择根骨骼", "Choose root bone", "選擇根骨骼")
                }
            }
            Label {
                objectName: "animRootStatus"
                Layout.fillWidth: true; Layout.minimumWidth: 0
                elide: Text.ElideRight
                color: Theme.muted
                font.pixelSize: Theme.noteText
                text: {
                    const info = root.session.rootLockInfo
                    if (!root.ready || !info || info.status === undefined) return ""
                    if (info.reason === "unmappedRoot")
                        return Theme.t("当前动画未映射此根，无法锁定", "This root is not mapped in the current animation; lock unavailable", "目前動畫未映射此根，無法鎖定")
                    if (info.status === "locked")
                        return Theme.t("预览已锁定", "Preview locked", "預覽已鎖定") + " · " + Theme.t("根", "root", "根") + " " + info.rootName + " · " +
                               (info.base === "reference" ? Theme.t("参考姿态", "reference", "參考姿態")
                                : info.base === "zero" ? Theme.t("零点", "zero", "零點")
                                : Theme.t("动画首帧", "anim first frame", "動畫首幀"))
                    if (info.status === "multiRoot")
                        return Theme.t("多个骨架根，需先选择根", "Multiple roots; choose one first", "多個骨架根，需先選擇根")
                    return Theme.t("未锁定 · 保留源根运动", "Unlocked · source root motion kept", "未鎖定 · 保留來源根運動")
                }
                ToolTip.visible: rootHover.hovered
                ToolTip.text: Theme.t("锁定仅影响预览：关闭在同一时间恢复源动作；导出不继承。", "Lock affects preview only; turning it off restores the source at the same time; exports never inherit it.", "鎖定僅影響預覽；關閉在同一時間還原來源動作；匯出不繼承。")
                HoverHandler { id: rootHover }
            }
            Tool {
                objectName: "animBarCollapse"
                text: Theme.t("收起", "Collapse", "收起")
                enabled: !root.session.busy
                onClicked: root.collapsed = true
                Accessible.name: Theme.t("收起时间轴以扩大预览", "Collapse the timeline to enlarge the preview", "收起時間軸以擴大預覽")
            }
            Tool {
                objectName: "animPairingMenuButton"
                visible: root.session.info.psa === true
                text: Theme.t("参考骨架…", "Reference…", "參考骨架…")
                enabled: !root.session.busy
                onClicked: pairingMenu.open()
                AppPopupMenu {
                    id: pairingMenu
                    y: parent.height
                    AppMenuItem { objectName: "animReference"; text: Theme.t("选择参考 PSK…", "Choose reference PSK…", "選擇參考 PSK…"); onTriggered: referenceDialog.open() }
                    AppMenuItem { objectName: "animCompanion"; text: Theme.t("选择轨道配置…", "Choose track config…", "選擇軌道設定…"); enabled: !!root.session.info.referencePath; onTriggered: companionDialog.open() }
                    AppMenuItem { objectName: "animReferenceClear"; text: Theme.t("清除配对", "Clear pairing", "清除配對"); enabled: !!root.session.info.referencePath; onTriggered: root.session.setAnimationReference("") }
                }
            }
        }
        Label {
            objectName: "animTime"
            Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.Wrap
            text: root.ready ? ("%1 s · %2 %3 / %4 · %5 %6 fps")
                .arg(root.session.time.toFixed(3)).arg(Theme.t("帧", "frame", "幀"))
                .arg(root.clipData.frame).arg(root.clipData.lastFrame)
                .arg(Theme.t("源", "src", "來源")).arg(root.clipData.fps) : ""
            color: Theme.text; font.family: "Consolas"
        }
        Slider {
            objectName: "animScrubber"
            Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredHeight: 32
            enabled: root.ready
            from: root.ready ? root.clipData.start : 0
            to: root.ready ? root.clipData.end : 1
            value: root.session.time
            onMoved: root.session.seek(value)
            Accessible.name: Theme.t("时间轴拖动", "Timeline scrub", "時間軸拖動")
        }
        Flow {
            Layout.fillWidth: true
            spacing: 12
            Label {
                objectName: "animStart"
                text: root.ready ? Theme.t("片段起点", "clip start", "片段起點") + " " + root.clipData.start.toFixed(3) + " s" : ""
                color: Theme.muted; font.pixelSize: Theme.noteText
            }
            Label {
                text: root.ready ? Theme.t("采样", "samples", "取樣") + " " + (root.clipData.lastFrame + 1) + " · " + Theme.t("索引", "indices", "索引") + " 0–" + root.clipData.lastFrame : ""
                color: Theme.muted; font.pixelSize: Theme.noteText
            }
            Label {
                objectName: "animEnd"
                text: root.ready ? Theme.t("结束边界", "end boundary", "結束邊界") + " " + root.clipData.end.toFixed(3) + " s" : ""
                color: Theme.tertiary; font.pixelSize: Theme.noteText
            }
        }
    }
    FileDialog {
        id: referenceDialog
        title: Theme.t("选择与 PSA 对应的参考骨架", "Choose the PSA reference skeleton", "選擇與 PSA 對應的參考骨架")
        nameFilters: ["ActorX (*.psk *.pskx)"]
        onAccepted: root.session.setAnimationReference(selectedFile.toString())
    }
    FileDialog {
        id: companionDialog
        title: Theme.t("选择 PSA 轨道配置", "Choose PSA track config", "選擇 PSA 軌道設定")
        nameFilters: ["PSA config (*.config)"]
        onAccepted: root.session.setAnimationCompanion(selectedFile.toString())
    }
}
