pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import FormaView.Video 1.0

FocusScope {
    id: root
    objectName: "videoPlayerPane"
    property string source
    property var playlistModel
    signal openVideo(string path)
    signal openAdjacent(int direction)
    signal closeVideo()
    readonly property alias player: video
    readonly property var stateMap: video.status
    readonly property real position: Number(stateMap["visible-time"] ?? stateMap["time-pos"] ?? 0)
    readonly property real duration: Number(stateMap.duration || 0)
    readonly property real speed: Number(stateMap.speed || 1)
    readonly property bool paused: !!stateMap.pause
    property real a: -1
    property real b: -1
    property string notice: ""
    property string mode: "normal"
    property string modeBeforeHdr: "normal"
    property var savedWindow
    property var hdrHost
    property var host
    property bool infoShown: false
    property bool controlsShown: true
    readonly property bool playlistVisible: mode === "normal" && prefs.playlistOpen && width >= 700
    property var audioTracks: (stateMap["track-list"] || []).filter(t => t.type === "audio")
    property var subtitles: (stateMap["track-list"] || []).filter(t => t.type === "sub")
    function format(t) {
        const ms = Math.max(0, Math.round(t * 1000));
        return Math.floor(ms / 3600000) + ":" + String(Math.floor(ms / 60000) % 60).padStart(2, "0") + ":" + String(Math.floor(ms / 1000) % 60).padStart(2, "0") + "." + String(ms % 1000).padStart(3, "0");
    }
    function rate(delta) {
        video.setOption("speed", Math.max(0.1, Math.min(8, Math.round((speed + delta) * 10) / 10)));
    }
    function revealControls() {
        controlsShown = true
        hideTimer.restart()
    }
    Component.onCompleted: revealControls()
    function focusPlayback() {
        surface.forceActiveFocus()
    }
    function textOrPopupFocused(): bool {
        const overlay = surface.Overlay.overlay
        for (let item = surface.Window.window?.activeFocusItem; item; item = item.parent) {
            if (item === overlay || item instanceof TextInput || item instanceof TextEdit)
                return true
        }
        return false
    }
    function setEndpoint(point, seconds) {
        const minimum = 0.001
        if (point === "A")
            a = Math.max(0, Math.min(b >= 0 ? b - minimum : duration, seconds))
        else if (a >= 0 && a + minimum <= duration)
            b = Math.max(a + minimum, Math.min(duration, seconds))
        if (b > a && a >= 0)
            video.loop(a, b)
    }
    function volume(delta) {
        video.setOption("volume", Math.min(100, Math.max(0, Number(stateMap.volume ?? 100) + delta)));
    }
    function shot() {
        video.screenshot(prefs.directory, prefs.captions);
    }
    function showNotice(s) {
        notice = s;
        messageTimer.restart();
    }
    function markA() {
        a = position;
        b = -1;
        video.clearLoop();
    }
    function markB() {
        if (a >= 0 && position > a) {
            b = position;
            video.loop(a, b);
        } else
            showNotice(qsTr("请先在较早位置设置 A"));
    }
    function changeMode(next) {
        if (mode === next)
            next = "normal";
        const w = host || root.Window.window;
        if (!w)
            return;
        if (mode === "hdr") {
            mode = modeBeforeHdr;
            video.leaveHdr();
            if (next === mode) {
                focusPlayback();
                return;
            }
        }
        if (next === "hdr") {
            const output = video.prepareHdrWindow();
            if (!output)
                return;
            host = w;
            hdrHost = output;
            modeBeforeHdr = mode;
            mode = "hdr";
            hdrHost.showFullScreen();
            hdrHost.requestActivate();
            focusPlayback();
            return;
        }
        if (mode !== "normal") {
            if (mode === "mini") {
                prefs.miniWidth = w.width;
                prefs.miniHeight = w.height;
                prefs.miniX = w.x;
                prefs.miniY = w.y;
            }
            w.minimumWidth = savedWindow.minW;
            w.minimumHeight = savedWindow.minH;
            w.showNormal();
            // Leave fullscreen before reapplying the main window's frame
            // hints, otherwise the platform can restore the borderless style.
            w.flags = savedWindow.flags;
            if (!video.restoreWindowPlacement(w, savedWindow.placement)) {
                w.x = savedWindow.x;
                w.y = savedWindow.y;
                w.width = savedWindow.width;
                w.height = savedWindow.height;
                if (savedWindow.maximized)
                    w.showMaximized();
            }
            mode = "normal";
            if (next === "normal") {
                // The expanded native title bar is recreated when the frame
                // flags return. Activate the playback window after that turn.
                Qt.callLater(() => {
                    if (root.mode === "normal" && w.visible)
                        w.requestActivate();
                });
            }
        }
        if (next !== "normal") {
            host = w;
            savedWindow = ({
                    flags: w.flags,
                    minW: w.minimumWidth,
                    minH: w.minimumHeight,
                    placement: video.windowPlacement(w),
                    x: w.x,
                    y: w.y,
                    width: w.width,
                    height: w.height,
                    maximized: w.visibility === Window.Maximized
                });
            mode = next;
            if (next === "full") {
                // The main window expands its client area into the native
                // title bar. Fullscreen must cover the monitor without those
                // non-client frame margins.
                w.flags = Qt.Window | Qt.FramelessWindowHint;
                w.showFullScreen();
                w.raise();
                w.requestActivate();
            } else {
                w.showNormal();
                w.flags = w.flags | Qt.WindowStaysOnTopHint;
                w.minimumWidth = 320;
                w.minimumHeight = 180;
                w.width = prefs.miniWidth;
                w.height = prefs.miniHeight;
                if (Qt.application.screens.some(s => prefs.miniX >= s.virtualX && prefs.miniX < s.virtualX + s.width && prefs.miniY >= s.virtualY && prefs.miniY < s.virtualY + s.height)) {
                    w.x = prefs.miniX;
                    w.y = prefs.miniY;
                }
                w.show();
            }
        }
        revealControls();
        surface.forceActiveFocus();
    }
    function subtitleDrop(urls) {
        if (urls.length !== 1 || !/\.(srt|ass|ssa|vtt)$/i.test(String(urls[0])))
            return false;
        video.addSubtitle(urls[0]);
        return true;
    }
    onSourceChanged: {
        if (source) Qt.callLater(() => {
            if (root.visible) {
                root.focusPlayback()
                root.revealControls()
            }
        })
        a = -1;
        b = -1;
        notice = "";
    }
    Component.onDestruction: if (mode !== "normal")
        changeMode("normal")
    Connections {
        target:root.hdrHost||null
        function onClosing(close){if(root.mode==="hdr"){close.accepted=false;root.changeMode("normal")}}
        function onScreenChanged(){if(root.mode==="hdr")root.changeMode("normal")}
    }
    Settings {
        id: prefs
        category: "Video"
        property url directory
        property bool captions: true
        property int miniX: -32768
        property int miniY: -32768
        property int miniWidth: 720
        property int miniHeight: 440
        property bool playlistOpen: true
    }
    component PlaybackMenu: Menu {
        id: popup
        required property Item trigger
        required property Item dock
        property int menuWidth: 206
        parent: dock
        popupType: Popup.Item
        width: Math.min(menuWidth, dock.width)
        height: Math.min(implicitHeight, Math.max(80, dock.y - 16))
        function alignToTrigger() {
            const right = trigger.mapToItem(dock, trigger.width, 0).x
            x = Math.max(0, Math.min(right - width, dock.width - width))
        }
        onAboutToShow: alignToTrigger()
        Connections {
            target: popup.dock
            function onWidthChanged() {
                if (popup.visible) Qt.callLater(popup.alignToTrigger)
            }
        }
        y: -height - 6
        padding: 6
        background: Rectangle {
            radius: 14
            border.color: "#52ffffff"
            gradient: Gradient {
                GradientStop { position: 0; color: "#ef303239" }
                GradientStop { position: 1; color: "#ed191a20" }
            }
        }
    }
    component PlaybackMenuItem: MenuItem {
        id: entry
        implicitHeight: 32
        height: visible ? implicitHeight : 0
        leftPadding: 12
        rightPadding: 12
        contentItem: Text {
            text: entry.text
            color: entry.enabled ? "#f5f5f7" : "#96969c"
            font.pixelSize: Theme.compactText
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 8
            color: entry.down ? "#43ffffff" : entry.highlighted ? "#2affffff" : "transparent"
            border.color: entry.visualFocus ? "#ffad67" : "transparent"
        }
    }
    Timer {
        id: messageTimer
        interval: 2800
        onTriggered: root.notice = ""
    }
    Timer {
        id: hideTimer
        interval: root.mode === "normal" ? 1200 : 850
        onTriggered: {
            if (!bar.interacting && bar.activeFocus)
                root.focusPlayback()
            root.controlsShown = false
        }
    }
    VideoPlaylist {
        id: playlist
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: root.playlistVisible ? Math.max(220, Math.min(300, root.width * 0.21)) : 0
        visible: root.playlistVisible
        model: root.playlistModel
        currentPath: root.source
        currentDuration: root.duration > 0 ? root.format(root.duration).slice(0, -4) : ""
        onOpenVideo: path => {
            if (path !== root.source)
                root.openVideo(path)
            root.focusPlayback()
        }
        onCollapse: {
            prefs.playlistOpen = false
            root.focusPlayback()
        }
    }
    Rectangle {
        id: surface
        layer.enabled: root.mode !== "normal" && (settings.visible || jump.visible || subDialog.visible || folder.visible)
        layer.effect: FrostEffect {}
        objectName: "videoViewport"
        parent: root.mode==="hdr" ? root.hdrHost.contentItem : root.mode === "normal" ? root : root.host.contentItem
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: root.mode === "normal" ? playlist.left : parent.right
        clip: true
        z: root.mode === "normal" ? 0 : 10000
        color: "#101114"
        focus: true
        Shortcut {
            sequence: "Tab"
            autoRepeat: false
            enabled: surface.visible && (root.mode !== "normal" || surface.activeFocus)
                     && !bar.keyboardFocus && !more.visible && !rates.visible && !settings.visible
                     && !jump.visible && !folder.visible && !subDialog.visible
            onActivated: root.infoShown = !root.infoShown
        }
        Keys.onPressed: e => {
            if(e.key===Qt.Key_G && e.modifiers===Qt.ControlModifier){jump.open();e.accepted=true;return}
            if (e.modifiers !== Qt.NoModifier)
                return;
            switch (e.key) {
            case Qt.Key_Up:
                root.volume(5);
                break;
            case Qt.Key_Down:
                root.volume(-5);
                break;
            case Qt.Key_Left:
                video.seek(-VideoPreferences.backward, true);
                break;
            case Qt.Key_Right:
                video.seek(VideoPreferences.forward, true);
                break;
            case Qt.Key_D:
            case Qt.Key_Comma:
                video.step(false);
                break;
            case Qt.Key_F:
            case Qt.Key_Period:
                video.step(true);
                break;
            case Qt.Key_BracketLeft:
                root.rate(-0.1);
                break;
            case Qt.Key_BracketRight:
                root.rate(0.1);
                break;
            case Qt.Key_1:
                video.setOption("speed", 1);
                break;
            case Qt.Key_S:
                root.shot();
                break;
            case Qt.Key_Escape:
                if (root.mode === "normal")
                    return;
                root.changeMode("normal");
                break;
            default:
                return;
            }
            e.accepted = true;
            root.revealControls();
        }
        VideoSession {
            id: video
            objectName: "videoSession"
            source: root.source
            playbackSpaceEnabled: surface.visible && surface.enabled && !root.textOrPopupFocused()
                                  && !(surface.Window.window?.frostDepth > 0)
                                  && !more.visible && !rates.visible && !settings.visible
                                  && !jump.visible && !folder.visible && !subDialog.visible
            onSourceChanged: {
                if (root.mode === "hdr") {
                    root.mode = root.modeBeforeHdr;
                    root.changeMode("normal");
                }
            }
            anchors.fill: parent
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Math.max(18, parent.height * 0.09, bar.visible ? bar.height + 22 : 0)
                width: parent.width * 0.9
                text: root.stateMap["review-caption"] || ""
                visible: text.length > 0
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: "white"
                style: Text.Outline
                styleColor: "black"
                font.pixelSize: Math.max(16, parent.height / 24)
            }
            onScreenshotSaved: path => root.showNotice(qsTr("截图已保存至 ") + path)
        }
        HoverHandler {
            blocking: false
            property point lastPosition: Qt.point(-1, -1)
            onPointChanged: {
                const p = point.position
                if (hovered && (p.x !== lastPosition.x || p.y !== lastPosition.y)) {
                    lastPosition = Qt.point(p.x, p.y)
                    root.revealControls()
                }
            }
            onHoveredChanged: {
                if (hovered) root.revealControls()
                else lastPosition = Qt.point(-1, -1)
            }
        }
        MouseArea {
            anchors.fill: video
            property bool pausedBeforeClick: false
            property string clickedSource
            onClicked: {
                pausedBeforeClick = root.paused
                clickedSource = root.source
                root.focusPlayback()
                video.togglePause()
            }
            onDoubleClicked: {
                // The first release has already toggled playback.
                if (clickedSource === root.source)
                    video.setOption("pause", pausedBeforeClick ? "yes" : "no")
                root.changeMode("full")
            }
        }
        Label {
            anchors.centerIn: video
            visible: !!video.error || !!root.notice
            text: root.notice || video.error
            color: "white"
            padding: 10
            background: Rectangle {
                color: "#cf202329"
                radius: 6
            }
        }
        Tool {
            objectName: "expandVideoPlaylist"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            visible: root.mode === "normal" && !root.playlistVisible && root.width >= 700
            iconName: "files-open"
            hint: qsTr("展开播放列表")
            onClicked: {
                prefs.playlistOpen = true
                root.focusPlayback()
            }
        }
        Tool {
            objectName: "returnFromVideoMini"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 8
            anchors.topMargin: 37
            visible: root.mode === "mini"
            iconName: "restore"
            hint: qsTr("返回主窗")
            onClicked: root.changeMode("normal")
        }
        Loader {
            id: infoLoader
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: Math.min(16, surface.width * 0.03)
            active: root.infoShown && surface.visible
            width: Math.min(540, surface.width - 2 * anchors.leftMargin)
            height: Math.max(0, Math.min(item ? item.implicitHeight : 0,
                        (bar.visible ? bar.y : surface.height) - 2 * anchors.topMargin))
            sourceComponent: PlaybackInfo {
                player: video
                fileName: root.source.split(/[\\/]/).pop()
                formatTime: root.format
            }
        }
        FocusScope {
            id: bar
            objectName: "videoGlassControls"
            readonly property real inset: Math.min(16, surface.width * 0.03)
            readonly property bool compact: width < 640
            readonly property bool narrowMini: root.mode === "mini" && width < 860
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: inset
            height: rows.implicitHeight + (compact ? 14 : 22)
            readonly property bool interacting: dockBackground.pressed || timeline.interacting || volumeSlider.pressed || more.visible || rates.visible
            readonly property bool keyboardFocus: activeFocus && !!surface.Window.window?.activeFocusItem?.visualFocus
            onInteractingChanged: if (!interacting) root.revealControls()
            visible: root.controlsShown || interacting
            Loader {
                anchors.fill: parent
                active: bar.visible && root.mode !== "hdr" && !!root.stateMap["video-params"] && !video.error
                sourceComponent: VideoControlGlass {
                    frame: video
                    panelOrigin: Qt.point(bar.x, bar.y)
                }
            }
            Rectangle {
                anchors.fill: parent
                radius: 22
                color: "#e5292b32"
                border.color: "#7a7d83"
                visible: root.mode === "hdr" || !root.stateMap["video-params"] || !!video.error
            }
            MouseArea {
                id: dockBackground
                anchors.fill: parent
                onPressed: root.revealControls()
            }
            ColumnLayout {
                id: rows
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: bar.compact ? 8 : 16
                anchors.rightMargin: bar.compact ? 8 : 16
                spacing: bar.compact ? 2 : 5
                VideoTimeline {
                    id: timeline
                    Layout.fillWidth: true
                    duration: root.duration
                    position: root.position
                    pointA: root.a
                    pointB: root.b
                    formatTime: seconds => root.format(seconds)
                    onSeekRequested: seconds => video.seek(seconds, false)
                    onEndpointMoved: (point, seconds) => root.setEndpoint(point, seconds)
                }
                Item {
                    id: actions
                    Layout.fillWidth: true
                    implicitHeight: bar.compact ? 33 : 44
                    Label {
                        id: timeLabel
                        objectName: "videoTimeReadout"
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, (pauseShot.visible ? pauseShot.x : transport.x) - 6)
                        elide: Text.ElideRight
                        font.family: "Consolas"
                        color: Theme.text
                        text: (root.paused && !bar.compact ? root.format(timeline.shownPosition) :
                            root.format(timeline.shownPosition).slice(0, -4)) + (bar.compact ? "" : " / " + root.format(root.duration).slice(0, -4))
                    }
                    RowLayout {
                        id: transport
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: bar.compact ? 0 : 6
                        Tool {
                            visible: !bar.compact
                            quiet: true
                            iconName: root.paused ? "animation-step-back" : "return"
                            hint: root.paused ? qsTr("上一帧 (D / ,)") : qsTr("回退 %1 秒").arg(VideoPreferences.backward)
                            onClicked: root.paused ? video.step(false) : video.seek(-VideoPreferences.backward, true)
                        }
                        Tool {
                            objectName: "videoPlayToggle"
                            quiet: true
                            Layout.preferredWidth: bar.compact ? 38 : 46
                            Layout.preferredHeight: bar.compact ? 32 : 40
                            contentItem: Item {
                                AppIcon {
                                    anchors.centerIn: parent
                                    name: root.paused ? "animation-play" : "animation-pause"
                                    size: 20
                                }
                            }
                            hint: qsTr("播放 / 暂停")
                            background: Rectangle {
                                radius: 11
                                color: parent.down ? "#60ffffff" : parent.hovered ? "#50ffffff" : "#37ffffff"
                                border.color: parent.visualFocus ? Theme.accent : "#8cd4d4d8"
                            }
                            onClicked: { video.togglePause(); root.focusPlayback() }
                        }
                        Tool {
                            visible: !bar.compact
                            quiet: true
                            iconName: root.paused ? "animation-step-forward" : "return"
                            iconRotation: root.paused ? 0 : 180
                            hint: root.paused ? qsTr("下一帧 (F / .)") : qsTr("快进 %1 秒").arg(VideoPreferences.forward)
                            onClicked: root.paused ? video.step(true) : video.seek(VideoPreferences.forward, true)
                        }
                    }
                    Tool {
                        id: pauseShot
                        visible: root.paused && !bar.compact
                        anchors.right: transport.left; anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        quiet: true; iconName: "save"; hint: qsTr("截图"); onClicked: root.shot()
                    }
                    RowLayout {
                        id: rightControls
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: bar.compact || bar.narrowMini ? 0 : 4
                        Tool {
                            objectName: "videoMiniButton"
                            quiet: true
                            iconName: "picture-in-picture"
                            checked: root.mode === "mini"
                            hint: root.mode === "mini" ? qsTr("返回主窗") : qsTr("置顶小窗")
                            onClicked: root.changeMode("mini")
                        }
                        Tool {
                            objectName: "videoMuteButton"
                            visible: bar.width >= 360
                            quiet: true
                            iconName: root.stateMap.mute || root.stateMap.volume === 0 ? "volume-muted" : "volume"
                            hint: root.stateMap.mute ? qsTr("取消静音") : qsTr("静音")
                            onClicked: video.setOption("mute", root.stateMap.mute ? "no" : "yes")
                        }
                        Slider {
                            id: volumeSlider
                            objectName: "videoVolume"
                            visible: bar.width >= 860 || (bar.narrowMini && bar.width >= 440)
                            Layout.preferredWidth: bar.narrowMini ? (bar.compact || bar.width >= 690 ? 52 : 40) : 72
                            Accessible.name: qsTr("音量")
                            from: 0
                            to: 100
                            stepSize: 1
                            value: root.stateMap.volume ?? 100
                            onMoved: {
                                video.setOption("volume", Math.round(value))
                                if (!pressed) volumeHintTimer.restart()
                                root.revealControls()
                            }
                            Timer { id: volumeHintTimer; interval: 600 }
                            Label {
                                objectName: "videoVolumeHint"
                                parent: volumeSlider.handle
                                visible: volumeSlider.pressed || volumeHintTimer.running
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.bottom: parent.top
                                anchors.bottomMargin: 5
                                z: 1
                                text: Math.round(volumeSlider.value) + "%"
                                color: "#fafafa"
                                leftPadding: 7
                                rightPadding: 7
                                topPadding: 2
                                bottomPadding: 2
                                background: Rectangle { color: "#ec25272c"; border.color: "#62767a82"; radius: 5 }
                            }
                            background: Rectangle {
                                x: volumeSlider.leftPadding
                                y: volumeSlider.height / 2 - 2
                                width: volumeSlider.availableWidth
                                height: 4
                                radius: 2
                                color: "#77777f"
                                Rectangle { width: parent.width * volumeSlider.visualPosition; height: parent.height; radius: 2; color: "#ffa764" }
                            }
                            handle: Rectangle {
                                x: volumeSlider.leftPadding + volumeSlider.availableWidth * volumeSlider.visualPosition - width / 2
                                y: volumeSlider.height / 2 - height / 2
                                implicitWidth: 12
                                implicitHeight: 12
                                radius: 6
                                color: "#ffad67"
                                border.color: volumeSlider.visualFocus ? "white" : "transparent"
                            }
                        }
                        Tool {
                            quiet: true
                            visible: !bar.compact
                            text: "CC"
                            leftPadding: bar.narrowMini ? 7 : 12
                            hint: qsTr("显示 / 隐藏字幕")
                            checkable: true
                            checked: root.subtitles.length > 0 && root.stateMap["sub-visibility"] !== false
                            onClicked: video.setOption("sub-visibility", checked ? "yes" : "no")
                        }
                        Tool {
                            quiet: true
                            visible: !bar.compact
                            id: rateButton
                            objectName: "videoRateButton"
                            text: root.speed.toFixed(1) + "×"
                            leftPadding: bar.narrowMini ? 7 : 12
                            hint: qsTr("播放速度")
                            onClicked: rates.open()
                            PlaybackMenu {
                                id: rates
                                objectName: "videoRateMenu"
                                dock: bar
                                trigger: rateButton
                                menuWidth: 146
                                PlaybackMenuItem { text: qsTr("加 0.1×"); onTriggered: root.rate(0.1) }
                                PlaybackMenuItem { text: qsTr("减 0.1×"); onTriggered: root.rate(-0.1) }
                                PlaybackMenuItem { text: "1.0×"; onTriggered: video.setOption("speed", 1) }
                            }
                        }
                        Tool {
                            id: menuButton
                            objectName: "videoMenuButton"
                            quiet: true
                            iconName: "more"
                            hint: qsTr("播放选项")
                            onClicked: more.open()
                        }
                        Tool {
                            objectName: "videoFullscreenButton"
                            quiet: true
                            iconName: root.mode === "normal" ? "fullscreen" : "restore"
                            hint: qsTr("全屏 / 返回")
                            onClicked: root.changeMode(root.mode === "normal" ? "full" : "normal")
                        }
                    }
                }
            }
        }
        PlaybackMenu {
            id: more
            PlaybackMenuItem {
                text: qsTr("播放信息") + "    Tab"
                checkable: true
                checked: root.infoShown
                onTriggered: { root.infoShown = !root.infoShown; root.focusPlayback() }
            }
            PlaybackMenuItem {text:qsTr("HDR10 全屏输出");visible:["pq","hlg"].includes(root.stateMap["video-params"]?.gamma);onTriggered:root.changeMode("hdr")}
            PlaybackMenuItem{text:qsTr("跳转到时间…  Ctrl+G");onTriggered:jump.open()}
            dock: bar
            trigger: menuButton
            PlaybackMenuItem {
                text: qsTr("截图")
                onTriggered: root.shot()
            }
            PlaybackMenuItem {
                text: qsTr("外挂字幕…")
                onTriggered: subDialog.present()
            }
            PlaybackMenuItem {
                text: qsTr("设 A  ·  ") + (root.a >= 0 ? root.format(root.a) : "—")
                onTriggered: root.markA()
            }
            PlaybackMenuItem {
                text: qsTr("设 B  ·  ") + (root.b >= 0 ? root.format(root.b) : "—")
                onTriggered: root.markB()
            }
            PlaybackMenuItem {
                text: qsTr("清除 A–B")
                enabled: root.a >= 0
                onTriggered: {
                    video.clearLoop();
                    root.a = -1;
                    root.b = -1;
                }
            }
            PlaybackMenuItem {
                text: root.mode === "mini" ? qsTr("返回主窗") : qsTr("置顶小窗")
                onTriggered: root.changeMode("mini")
            }
            PlaybackMenuItem {
                text: qsTr("上一个视频")
                onTriggered: root.openAdjacent(-1)
            }
            PlaybackMenuItem {
                text: qsTr("下一个视频")
                onTriggered: root.openAdjacent(1)
            }
            PlaybackMenuItem {
                text: qsTr("播放设置…")
                onTriggered: settings.open()
            }
            PlaybackMenuItem {
                visible: root.mode === "normal"
                text: qsTr("关闭当前文件")
                onTriggered: root.closeVideo()
            }
        }
        FrostDialog {
            id: settings
            title: qsTr("播放设置")
            anchors.centerIn: parent
            modal: true
            width: Math.min(440, surface.width)
            height: Math.min(530, surface.height)
            standardButtons: Dialog.Close
            contentItem: ScrollView {
                clip: true
                ColumnLayout {
                    width: settings.availableWidth - 16
                    ComboBox {
                        visible: count > 0
                        Layout.fillWidth: true
                        model: (root.stateMap["chapter-list"] || []).map((c, i) => c.title || qsTr("章节 ") + (i + 1))
                        currentIndex: Number(root.stateMap.chapter ?? -1)
                        onActivated: i => video.setOption("chapter", i)
                    }
                    Label {
                        text: qsTr("音轨")
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        model: root.audioTracks
                        textRole: "title"
                        displayText: currentIndex >= 0 ? String(root.audioTracks[currentIndex]?.lang || qsTr("音轨")) + " " + String(root.audioTracks[currentIndex]?.id) : "—"
                        currentIndex: root.audioTracks.findIndex(t => t.selected)
                        delegate: ItemDelegate {
                            required property var modelData
                            text: (modelData.title || modelData.lang || qsTr("音轨")) + " " + modelData.id
                        }
                        onActivated: i => video.setOption("aid", root.audioTracks[i].id)
                    }
                    Label {
                        text: qsTr("字幕")
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        model: [qsTr("关闭")].concat(root.subtitles.map(t => (t.title || t.lang || qsTr("字幕")) + " " + t.id))
                        currentIndex: root.subtitles.findIndex(t => t.selected) + 1
                        onActivated: i => video.setOption("sid", i === 0 ? "no" : root.subtitles[i - 1].id)
                    }
                    RowLayout {
                        Label {
                            text: qsTr("字幕延迟（毫秒）")
                        }
                        SpinBox {
                            from: -60000
                            to: 60000
                            stepSize: 100
                            editable: true
                            value: Math.round(Number(root.stateMap["sub-delay"] || 0) * 1000)
                            onValueModified: video.setOption("sub-delay", value / 1000)
                        }
                    }
                    RowLayout {
                        Label {
                            text: qsTr("回退 / 快进（秒）")
                        }
                        SpinBox {
                            from: 1
                            to: 600
                            value: VideoPreferences.backward
                            onValueModified: VideoPreferences.backward = value
                        }
                        SpinBox {
                            from: 1
                            to: 600
                            value: VideoPreferences.forward
                            onValueModified: VideoPreferences.forward = value
                        }
                    }
                    CheckBox {
                        text: qsTr("截图包含字幕")
                        checked: prefs.captions
                        onToggled: prefs.captions = checked
                    }
                    Button {
                        text: qsTr("选择截图目录…")
                        onClicked: folder.present()
                    }
                    CheckBox {
                        text: qsTr("单文件循环")
                        onToggled: video.setOption("loop-file", checked ? "inf" : "no")
                    }
                    CheckBox {
                        text: qsTr("原始大小")
                        onToggled: video.setOption("video-unscaled", checked ? "yes" : "no")
                    }
                    Repeater {
                        id: filterRows
                        model: [["brightness", qsTr("亮度")], ["contrast", qsTr("对比度")], ["saturation", qsTr("饱和度")], ["sharpen", qsTr("锐化")]]
                        RowLayout {
                            required property var modelData
                            function reset() { adjust.value = 0; video.setOption(modelData[0], 0) }
                            Label {
                                text: modelData[1]
                            }
                            Slider {
                                id: adjust
                                from: modelData[0] === "sharpen" ? 0 : -100
                                to: modelData[0] === "sharpen" ? 2 : 100
                                value: 0
                                onMoved: video.setOption(modelData[0], value)
                            }
                            Tool {
                                text: "↺"
                                onClicked: {
                                    adjust.value = 0;
                                    video.setOption(modelData[0], 0);
                                }
                            }
                        }
                    }
                    CheckBox {
                        id: grayscale
                        text: qsTr("灰度")
                        onToggled: video.setOption("saturation", checked ? -100 : 0)
                    }
                    Button {
                        text: qsTr("重置全部滤镜")
                        onClicked: {
                            grayscale.checked = false
                            for (let i=0; i<filterRows.count; ++i) filterRows.itemAt(i).reset()
                        }
                    }
                    CheckBox {
                        text: qsTr("HDMI 杜比直通")
                        onToggled: video.setOption("audio-spdif", checked ? "ac3,eac3,truehd" : "")
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("视频 ") + (root.stateMap["video-params"]?.gamma || "—") + (root.stateMap["hdr-output"]?" → HDR10/PQ · ":" → SDR · ") + (root.stateMap["hwdec-current"] || "—") + "\n" + qsTr("音频 ") + (root.stateMap["audio-codec-name"] || "—") + " → " + (root.stateMap["audio-out-params"]?.format || "—")
                    }
                }
            }
        }
        FrostDialog {
            id:jump;title:qsTr("跳转到时间");anchors.centerIn:parent;modal:true
            standardButtons:Dialog.Ok|Dialog.Cancel
            onOpened:{when.text=root.position.toFixed(3);when.forceActiveFocus();when.selectAll()}
            onAccepted:{video.seek(Number(when.text),false);surface.forceActiveFocus()}
            TextField{id:when;placeholderText:qsTr("秒，例如 398.000");validator:DoubleValidator{bottom:0;locale:"C";notation:DoubleValidator.StandardNotation}
                onAccepted:if(acceptableInput)jump.accept()
            }
        }
        FrostFileDialog {
            parentWindow: surface.Window.window
            id: subDialog
            nameFilters: ["Subtitles (*.srt *.ass *.ssa *.vtt)"]
            onAccepted: video.addSubtitle(selectedFile)
        }
        FrostFolderDialog {
            parentWindow: surface.Window.window
            id: folder
            onAccepted: prefs.directory = selectedFolder
        }
    }
}
