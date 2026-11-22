pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: panel
    required property var player
    required property string fileName
    required property var formatTime
    readonly property var stats: player.playbackInfo
    readonly property var video: stats["video-dec-params"] || stats["video-params"] || ({})
    readonly property var output: stats["video-out-params"] || ({})
    readonly property var audio: stats["audio-params"] || ({})
    readonly property var speakers: stats["audio-out-params"] || ({})
    readonly property var streams: stats["track-list"] || []
    readonly property var videoTrack: streams.find(t => t.type === "video" && t.selected) || ({})
    readonly property var audioTrack: streams.find(t => t.type === "audio" && t.selected) || ({})
    readonly property real position: stats["visible-time"] ?? stats["time-pos"] ?? 0
    property string localTime
    objectName: "videoPlaybackInfo"
    radius: 14
    color: "#e61c2027"
    border.color: "#54ffffff"
    implicitHeight: content.implicitHeight + 26

    function value(v) { return v == null || v === "" ? "—" : String(v) }
    function num(v, unit, digits = 0, scale = 1) {
        return v == null || !Number.isFinite(Number(v)) ? "—" : (Number(v) * scale).toFixed(digits) + unit
    }
    function dimensions(p) { return p.w && p.h ? p.w + " × " + p.h : "—" }
    function audioFormat(p) {
        return [p.format, num(p.samplerate, " Hz"), p["hr-channels"] || p.channels].map(value).join(" · ")
    }
    function sample() {
        localTime = Qt.formatTime(new Date(), "HH:mm:ss")
        player.samplePlaybackInfo()
    }
    Timer { interval: 1000; running: panel.visible; repeat: true; triggeredOnStart: true; onTriggered: panel.sample() }
    Component.onDestruction: if (player) player.clearPlaybackInfo()

    component Line: RowLayout {
        id: line
        required property string caption
        required property string detail
        Layout.fillWidth: true
        spacing: 8
        Label { Layout.preferredWidth: panel.width < 380 ? 78 : 108; Layout.alignment: Qt.AlignTop; text: line.caption; color: "#b6c2d0"; font.pixelSize: 12; wrapMode: Text.Wrap }
        Label { Layout.fillWidth: true; text: line.detail; textFormat: Text.PlainText; color: "#f4f6fa"; font.pixelSize: 13; wrapMode: Text.WrapAnywhere }
    }
    component Section: Label {
        Layout.topMargin: 6
        font.bold: true; font.pixelSize: 13; color: "#ffb878"
    }
    Flickable {
        anchors.fill: parent; anchors.margins: 13
        contentWidth: width; contentHeight: content.implicitHeight
        clip: true; interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: content
            width: parent.width - 7
            spacing: 4
            Label { Layout.fillWidth: true; text: panel.fileName; textFormat: Text.PlainText; elide: Text.ElideMiddle; color: "#ffffff"; font.bold: true; font.pixelSize: 15 }
            Line { caption: qsTr("时间轴"); detail: panel.formatTime(panel.position) + " / " + (panel.stats.duration != null ? panel.formatTime(panel.stats.duration) : "—") + "  ·  " + panel.num(panel.stats.duration > 0 ? panel.position / panel.stats.duration : null, "%", 1, 100) }
            Line { caption: qsTr("时钟 / 帧号"); detail: panel.localTime + "  ·  " + qsTr("估算") + " " + panel.value(panel.stats["visible-time"] != null ? (panel.stats["container-fps"] ? Math.round(panel.position * panel.stats["container-fps"]) : null) : panel.stats["estimated-frame-number"]) + " / " + panel.value(panel.stats["estimated-frame-count"]) }
            Section { text: qsTr("视频") }
            Line { caption: qsTr("解码器"); detail: panel.value(panel.stats["video-codec"]) + " · " + panel.value(panel.stats["hwdec-current"]) }
            Line { caption: qsTr("源画面"); detail: panel.dimensions(panel.video) + " · " + panel.value(panel.videoTrack.codec) + " · " + panel.value(panel.video.pixelformat) + (panel.video["hw-pixelformat"] ? " / " + panel.video["hw-pixelformat"] : "") + (panel.video["plane-depth"] ? " · " + panel.video["plane-depth"] + " bit" : "") + " · " + panel.num(panel.stats["container-fps"], " fps", 2) + " · " + panel.num(panel.stats["video-bitrate"] ?? panel.videoTrack["demux-bitrate"], " kbps", 0, 0.001) }
            Line { caption: qsTr("范围 / 色彩"); detail: [panel.video["colorlevels"], panel.video.primaries, panel.video.gamma, panel.video.colormatrix].map(panel.value).join(" · ") }
            Line { caption: qsTr("滤镜输出"); detail: panel.dimensions(panel.output) + " · " + panel.value(panel.output.pixelformat) + " · " + panel.num(panel.stats["estimated-vf-fps"], " fps", 2) }
            Line { caption: qsTr("渲染目标"); detail: "libmpv / Qt Quick D3D11 · " + (panel.stats["hdr-output"] ? "RGB10A2 · HDR10" : "RGBA8 · SDR") + " · " + panel.num(panel.stats["screen-hz"], " Hz", 1) }
            Line { caption: qsTr("丢帧"); detail: qsTr("解码 %1  ·  输出 %2  ·  延迟 %3").arg(panel.value(panel.stats["decoder-frame-drop-count"])).arg(panel.value(panel.stats["frame-drop-count"])).arg(panel.value(panel.stats["vo-delayed-frame-count"])) }
            Line { caption: qsTr("交换链"); detail: panel.value(panel.stats["swap-effect"]) + " · " + panel.value(panel.stats["swap-format"]) + " · " + panel.value(panel.stats["swap-width"]) + " × " + panel.value(panel.stats["swap-height"]) + " · " + qsTr("%1 缓冲").arg(panel.value(panel.stats.buffers)) }
            Line { caption: qsTr("呈现 / 刷新"); detail: qsTr("提交 %1 · 呈现 %2 · 刷新 %3 · 同步 %4").arg(panel.value(panel.stats.submitted)).arg(panel.value(panel.stats.presented)).arg(panel.value(panel.stats["refresh-count"])).arg(panel.value(panel.stats["sync-count"])) }
            Line { caption: qsTr("同步时序"); detail: qsTr("垂直同步抖动 %1 · 错时帧 %2").arg(panel.num(panel.stats["vsync-jitter"], "%", 2, 100)).arg(panel.value(panel.stats["mistimed-frame-count"])) }
            Section { text: qsTr("音频") }
            Line { caption: qsTr("解码器 / 输入"); detail: panel.audioTrack.id == null ? qsTr("无选中音轨") : panel.value(panel.stats["audio-codec"] || panel.audioTrack.codec) + " · " + panel.audioFormat(panel.audio) + " · " + panel.num(panel.stats["audio-bitrate"] ?? panel.audioTrack["demux-bitrate"], " kbps", 0, 0.001) }
            Line { caption: qsTr("音频输出"); detail: panel.audioFormat(panel.speakers) + " · " + panel.value(panel.stats["current-ao"]) }
            Line { caption: qsTr("音画偏移 / 缓存"); detail: panel.num(panel.stats.avsync, " ms", 1, 1000) + " · " + qsTr("解复用缓存") + " " + panel.num(panel.stats["demuxer-cache-duration"], " s", 2) }
            Line { caption: qsTr("音频缓冲目标"); detail: panel.num(panel.stats["audio-buffer"], " ms", 0, 1000) }
            Section { text: qsTr("系统与设备") }
            Line { caption: qsTr("CPU"); detail: qsTr("进程 %1  ·  系统 %2").arg(panel.num(panel.stats["cpu-process"], "%", 1)).arg(panel.num(panel.stats["cpu-system"], "%", 1)) }
            Line { caption: "GPU"; detail: panel.value(panel.stats.adapter) + " · " + panel.num(panel.stats["gpu-busy"], "%", 1) + " · VPE " + panel.num(panel.stats["vpe-busy"], "%", 1) + " · " + panel.num(panel.stats["gpu-mhz"], " MHz") }
            Line { caption: qsTr("进程显存 / 预算"); detail: panel.num(panel.stats.vram, " MiB", 0, 1/1048576) + " / " + panel.num(panel.stats["vram-budget"], " MiB", 0, 1/1048576) }
            Line { caption: qsTr("版本"); detail: "FormaView " + Qt.application.version + " · " + panel.value(panel.stats["mpv-version"]) + " · FFmpeg " + panel.value(panel.stats["ffmpeg-version"]) + " · " + panel.value(panel.stats.os) }
        }
    }
}
