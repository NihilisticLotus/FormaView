pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var controller
    required property var session
    property Item modelPane
    signal locateTexture()
    readonly property var material: session.materialInfo
    readonly property var request: session.textureRequestInfo
    readonly property var images: {
        controller.addedAssets; controller.directory; controller.count
        return [...new Set([...(controller.libraryFileGroups("added", "").image || []),
                            ...(controller.libraryFileGroups("directory", "").image || [])])]
    }
    property string selectedImage: ""
    function associate(path: string): void {
        if (modelPane) modelPane.previewMode = 0
        session.associateTexture(path)
    }
    spacing: 10
    Label { text: Theme.t("材质与关联资源", "Materials and resources", "材質與關聯資源"); color: Theme.text; font.bold: true }
    Label {
        Layout.fillWidth: true; visible: root.session.materials.length === 0
        color: Theme.muted; wrapMode: Text.Wrap
        text: Theme.t("当前网格没有材质槽。", "This mesh has no material slots.", "目前網格沒有材質槽。")
    }
    Choice {
        objectName: "modelMaterialChoice"; Layout.fillWidth: true
        model: root.session.materials; currentIndex: root.session.materialIndex
        enabled: count > 0; onActivated: root.session.selectMaterial(currentIndex)
        Accessible.name: Theme.t("材质槽", "Material slot", "材質槽")
    }
    Readout { Layout.fillWidth: true; label: Theme.t("当前网格", "Current mesh", "目前網格"); value: root.material.mesh || "—" }
    Readout { Layout.fillWidth: true; label: Theme.t("源基础颜色贴图", "Source base colour texture", "來源基礎色貼圖"); value: root.session.textureReference || Theme.t("未提供", "Not provided", "未提供") }
    Readout { objectName: "modelTexturePath"; Layout.fillWidth: true; label: Theme.t("已验证贴图", "Validated texture", "已驗證貼圖"); value: root.session.texturePath || "—" }
    Choice {
        objectName: "modelTextureUV"; Layout.fillWidth: true
        model: root.session.uvSets; currentIndex: root.session.uvIndex
        enabled: count > 0; onActivated: root.session.selectUV(currentIndex)
        Accessible.name: Theme.t("关联 UV 集", "Association UV set", "關聯 UV 集")
    }
    Label {
        Layout.fillWidth: true; visible: !root.material.canAssociate
        color: Theme.muted; wrapMode: Text.Wrap; font.pixelSize: Theme.noteText
        text: Theme.t("当前网格没有可用的所选 UV 集，无法关联二维贴图。", "This mesh has no usable selected UV set for a texture.", "目前網格沒有可用的所選 UV 集，無法關聯二維貼圖。")
    }
    Choice {
        objectName: "modelWorkspaceTexture"; Layout.fillWidth: true
        labels: [Theme.t("选择工作区图片…", "Choose a workspace image…", "選取工作區圖片…"), ...root.images.map(path => path.split(/[\\/]/).pop())]
        notes: ["", ...root.images]
        currentIndex: root.images.indexOf(root.selectedImage) + 1
        enabled: root.images.length > 0
        onActivated: index => root.selectedImage = root.images[index - 1] || ""
        Accessible.name: Theme.t("工作区图片", "Workspace image", "工作區圖片")
    }
    Tool {
        objectName: "modelApplyWorkspaceTexture"; Layout.fillWidth: true
        text: Theme.t("关联所选图片", "Associate selected image", "關聯所選圖片")
        enabled: !!root.material.canAssociate && root.images.includes(root.selectedImage) && !root.session.textureBusy
        onClicked: root.associate(root.selectedImage)
    }
    Tool {
        objectName: "modelLocateTexture"; Layout.fillWidth: true
        text: Theme.t("定位贴图…", "Locate texture…", "定位貼圖…")
        enabled: !!root.material.canAssociate && !root.session.busy && !root.session.textureBusy
        onClicked: root.locateTexture()
    }
    Tool {
        objectName: "modelClearTexture"; Layout.fillWidth: true
        text: Theme.t("清除会话关联", "Clear session association", "清除工作階段關聯")
        enabled: root.session.canClearTexture && !root.session.textureBusy
        onClicked: root.session.removeTexture()
    }
    Label {
        objectName: "modelTextureStatus"; Layout.fillWidth: true
        visible: !!root.request.status || root.session.textureBusy; wrapMode: Text.Wrap; color: Theme.muted
        text: root.session.textureBusy && !root.request.status ? Theme.t("正在读取另一材质的贴图…", "Reading another material's texture…", "正在讀取另一材質的貼圖…")
            : root.request.status === "loading" ? Theme.t("正在解码贴图…", "Decoding texture…", "正在解碼貼圖…")
            : root.request.status === "cancelled" ? Theme.t("读取已取消，保留之前的显示。", "Read cancelled; previous display retained.", "讀取已取消，保留先前顯示。")
            : root.request.status === "error" ? Theme.t("读取失败，保留之前的显示。", "Read failed; previous display retained.", "讀取失敗，保留先前顯示。")
            : Theme.t("会话贴图已应用。", "Session texture applied.", "工作階段貼圖已套用。")
    }
    Readout { Layout.fillWidth: true; visible: !!root.request.status && root.request.status !== "ready"; label: Theme.t("请求文件", "Requested file", "請求檔案"); value: root.request.path || "" }
    Tool {
        objectName: "modelCancelTexture"; Layout.fillWidth: true; visible: root.session.textureBusy
        text: Theme.t("取消贴图读取", "Cancel texture read", "取消貼圖讀取")
        onClicked: root.session.cancelTexture()
    }
    Tool {
        objectName: "modelRetryTexture"; Layout.fillWidth: true
        visible: root.request.status === "error" || root.request.status === "cancelled"
        enabled: !root.session.textureBusy
        text: Theme.t("重试贴图读取", "Retry texture read", "重試貼圖讀取")
        onClicked: { if (root.modelPane) root.modelPane.previewMode = 0; root.session.retryTexture() }
    }
    ExplainedText { objectName: "modelTextureError"; Layout.fillWidth: true; kind: "error"; raw: root.request.status ? root.session.textureError : "" }
    SectionHeader { id: uvDetails; objectName: "modelSourceTextureUV"; Layout.fillWidth: true; text: Theme.t("源贴图 UV 变换", "Source texture UV transform", "來源貼圖 UV 變換") }
    ColumnLayout {
        Layout.fillWidth: true; visible: uvDetails.checked
        Readout { Layout.fillWidth: true; label: Theme.t("源 UV 集", "Source UV set", "來源 UV 集"); value: root.material.sourceUV || Theme.t("未提供", "Not provided", "未提供") }
        Readout {
            objectName: "modelTextureTransform"; Layout.fillWidth: true
            label: Theme.t("UV 仿射矩阵（2 × 3）", "UV affine matrix (2 × 3)", "UV 仿射矩陣（2 × 3）")
            value: root.material.uvTransform?.length === 6
                ? root.material.uvTransform.slice(0, 3).map(v => Number(v).toFixed(4)).join("  ") + "\n" + root.material.uvTransform.slice(3).map(v => Number(v).toFixed(4)).join("  ")
                : Theme.t("源文件未提供", "Not provided by the source", "來源未提供")
        }
    }
    Label {
        Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap; font.pixelSize: Theme.noteText
        text: Theme.t("关联仅作用于当前网格的材质槽。清除后恢复源贴图与参数，不修改源文件。", "Association affects this mesh's material slot only. Clearing restores the source texture and parameters; source files stay unchanged.", "關聯僅作用於目前網格的材質槽。清除後恢復來源貼圖與參數，不修改來源檔案。")
    }
}
