pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var session
    property Item modelPane
    readonly property var camera: session.cameraInfo
    function number(value, unit = ""): string {
        return value === undefined || value === null || !isFinite(Number(value))
            ? Theme.t("未提供", "Not provided", "未提供") : Number(value).toFixed(3) + unit
    }
    spacing: 10
    SectionHeader { id: heading; objectName: "modelCameras"; Layout.fillWidth: true; checked: true; text: Theme.t("资产相机", "Asset cameras", "資產相機") }
    ColumnLayout {
        Layout.fillWidth: true; visible: heading.checked
        Label { Layout.fillWidth: true; visible: root.session.cameras.length === 0; color: Theme.muted; wrapMode: Text.Wrap; text: Theme.t("当前场景没有资产相机。", "This scene has no asset cameras.", "目前場景沒有資產相機。") }
        ColumnLayout {
            Layout.fillWidth: true; visible: root.session.cameras.length > 0
            Choice {
                objectName: "modelCameraChoice"; Layout.fillWidth: true
                model: root.session.cameras; currentIndex: root.session.cameraIndex
                onActivated: index => root.session.selectCamera(index)
                Accessible.name: Theme.t("选择资产相机", "Select asset camera", "選取資產相機")
            }
            Readout { Layout.fillWidth: true; label: Theme.t("投影", "Projection", "投影"); value: root.camera.projection === "perspective" ? Theme.t("透视", "Perspective", "透視") : root.camera.projection === "orthographic" ? Theme.t("正交", "Orthographic", "正交") : "—" }
            Readout { Layout.fillWidth: true; label: Theme.t("源宽高比", "Source aspect ratio", "來源寬高比"); value: root.camera.aspect ? root.number(root.camera.aspect) : Theme.t("未指定 · 跟随视口", "Unspecified · follows viewport", "未指定 · 跟隨視口") }
            Readout { Layout.fillWidth: true; label: Theme.t("当前时间", "Current time", "目前時間"); value: root.number(root.camera.time, " s") }
            Readout { Layout.fillWidth: true; label: Theme.t("源世界位置", "Source world position", "來源世界位置"); value: root.camera.position || "—" }
            OptionCheck { objectName: "modelCameraFrustum"; text: Theme.t("显示相机视锥", "Show camera frustum", "顯示相機視錐"); checked: root.session.cameraFrustum; onToggled: root.session.cameraFrustum = checked }
            OptionCheck { objectName: "modelCameraPath"; text: Theme.t("显示相机轨迹", "Show camera path", "顯示相機軌跡"); checked: root.session.cameraPath; onToggled: root.session.cameraPath = checked }
            Label { Layout.fillWidth: true; visible: root.session.cameraPath; color: Theme.muted; wrapMode: Text.Wrap; text: !root.camera.pathCount ? Theme.t("相机轨迹不可用", "Camera path unavailable", "相機軌跡不可用") : root.camera.pathMoving ? Theme.t("当前片段的位置轨迹", "Position path of the current clip", "目前片段的位置軌跡") : Theme.t("当前片段没有位置变化", "No position changes in this clip", "目前片段沒有位置變化") }
            Tool {
                objectName: "modelCameraView"; Layout.fillWidth: true
                enabled: root.session.cameraView || !!root.camera.valid
                text: root.session.cameraView ? Theme.t("返回自由视图", "Return to free view", "返回自由視圖") : Theme.t("进入相机视图", "Enter camera view", "進入相機視圖")
                onClicked: {
                    const enter = !root.session.cameraView
                    if (enter && root.modelPane) root.modelPane.selectSection("model", 0)
                    root.session.cameraView = enter
                }
            }
            Tool { objectName: "modelFrameCamera"; Layout.fillWidth: true; enabled: !!root.camera.valid; text: Theme.t("框选相机与模型", "Frame camera and model", "框選相機與模型"); onClicked: root.modelPane?.frameSourceCamera() }
            Label { Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap; text: Theme.t("视锥与轨迹显示在自由视图中。时间轴同步相机姿态；源读数不包含居中偏移或根运动锁定。", "Guides appear in free view. The timeline drives the camera; source readings exclude centering and root lock.", "視錐與軌跡顯示於自由視圖。時間軸同步相機姿態；來源讀數不含居中偏移或根運動鎖定。") }
            Label { Layout.fillWidth: true; visible: !root.camera.valid; color: Theme.muted; wrapMode: Text.Wrap; text: Theme.t("源变换或投影参数不可用，无法进入相机视图。", "Source transform or projection unavailable; camera view is disabled.", "來源變換或投影參數不可用，無法進入相機視圖。") }
            SectionHeader { id: details; Layout.fillWidth: true; text: Theme.t("镜头与来源", "Lens and source", "鏡頭與來源") }
            ColumnLayout {
                Layout.fillWidth: true; visible: details.checked
                Readout { Layout.fillWidth: true; label: Theme.t("源路径", "Source path", "來源路徑"); value: root.camera.path || "—" }
                Readout { Layout.fillWidth: true; label: Theme.t("局部轴向", "Local axes", "局部軸向"); value: root.camera.axes || "—" }
                Readout { Layout.fillWidth: true; label: "FOV X / Y"; value: root.number(root.camera.fovX, "°") + " / " + root.number(root.camera.fovY, "°") }
                Readout { Layout.fillWidth: true; visible: root.camera.projection === "orthographic"; label: Theme.t("正交视野", "Orthographic span", "正交視野"); value: root.number(root.camera.spanX) + " / " + root.number(root.camera.spanY) }
                Readout { Layout.fillWidth: true; label: Theme.t("近 / 远裁剪", "Near / far clipping", "近 / 遠裁剪"); value: root.number(root.camera.near) + " / " + (root.camera.infiniteFar ? "∞" : root.number(root.camera.far)) }
                Readout { Layout.fillWidth: true; label: Theme.t("焦距 / Filmback", "Focal length / Filmback", "焦距 / Filmback"); value: root.number(root.camera.focalMm, " mm") + " / " + root.number(root.camera.apertureXmm) + " × " + root.number(root.camera.apertureYmm, " mm") }
                Readout { Layout.fillWidth: true; label: Theme.t("轨迹采样点", "Path samples", "軌跡取樣點"); value: String(root.camera.pathCount || 0) }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; textFormat: Text.PlainText; text: (root.camera.fovSource || "") + " · " + (root.camera.unit || "") }
            }
        }
    }
}
