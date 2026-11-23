import QtQuick
import QtQuick.Controls.Basic

Choice {
    id: control
    required property var session
    model: session.clips
    textRole: "name"
    currentIndex: session.clipIndex
    // ComboBox initialization/model replacement may reset the index after the
    // session notification. Reapply its binding once that reset has completed.
    function syncIndex() { currentIndex = Qt.binding(() => session.clipIndex) }
    onModelChanged: Qt.callLater(syncIndex)
    Component.onCompleted: syncIndex()
    enabled: count > 0
    labels: model.map(clip => clip.sourcePath ? clip.name + " · " + clip.sourcePath.split("/").pop() : clip.name)
    notes: model.map(clip => clip.sourcePath || "")
    displayText: labels[currentIndex] || Theme.t("无动画片段", "No animation clips", "無動畫片段")
    Accessible.name: Theme.t("动画片段", "Animation clip", "動畫片段")
    ToolTip.visible: hovered && count > 0
    ToolTip.text: noteAt(currentIndex) ? currentText + "\n" + noteAt(currentIndex) : currentText
    onActivated: index => session.setClipIndex(index)
}
