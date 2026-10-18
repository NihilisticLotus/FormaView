import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
Dialog {
    id: root
    required property var controller
    title: Theme.t("MCP 设置", "MCP settings", "MCP 設定")
    anchors.centerIn: Overlay.overlay
    width: 620; modal: true; focus: true
    standardButtons: Dialog.Close
    ColumnLayout {
        anchors.fill: parent; spacing: 16
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.accent; text: Theme.t("未启动 · MCP 服务尚未接入", "Not running · MCP service is not connected", "未啟動 · MCP 服務尚未接入") }
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("以下仅保存目录偏好，不授予访问权限，也不会启动服务。启用服务时仍需验证范围。", "These folder preferences do not grant access or start a service. Scope must be validated when the service becomes available.", "以下僅儲存資料夾偏好，不授予存取權限，也不會啟動服務。啟用服務時仍須驗證範圍。") }
        Label { text: Theme.t("输入目录偏好", "Preferred input folder", "輸入資料夾偏好"); color: Theme.text }
        Field { Layout.fillWidth: true; text: root.controller.preferences.mcpInputRoot || ""; onEditingFinished: root.controller.preference("mcpInputRoot", text); Accessible.name: Theme.t("输入目录偏好", "Preferred input folder", "輸入資料夾偏好") }
        Label { text: Theme.t("输出目录偏好", "Preferred output folder", "輸出資料夾偏好"); color: Theme.text }
        Field { Layout.fillWidth: true; text: root.controller.preferences.mcpOutputRoot || ""; onEditingFinished: root.controller.preference("mcpOutputRoot", text); Accessible.name: Theme.t("输出目录偏好", "Preferred output folder", "輸出資料夾偏好") }
    }
}
