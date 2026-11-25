pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ScrollView {
    id: root
    objectName: "mcpSettingsScroll"
    required property var controller
    readonly property var service: controller.imageJobs
    readonly property bool editable: !service.mcpEnabled && !service.mcpStarting
    clip: true
    contentWidth: availableWidth
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() { if (root.visible) Qt.callLater(() => { const window = root.Window.window; if (window) Theme.revealIn(root, window.activeFocusItem) }) }
    }
    ColumnLayout {
        width: root.availableWidth
        spacing: 16
        SettingsGroup {
            Layout.fillWidth: true
            title: Theme.t("服务与连接", "Service and connection", "服務與連線")
            SettingsRow {
                label: "MCP"
                description: root.service.mcpStarting ? Theme.t("正在启动…", "Starting…", "正在啟動…")
                    : !root.service.mcpEnabled ? Theme.t("已关闭", "Off", "已關閉")
                    : root.service.lastClient.name ? Theme.t("最近访问的客户端", "Last active client", "最近存取的用戶端") + " · " + root.service.lastClient.name
                    : Theme.t("已开启，等待客户端访问", "On, waiting for a client", "已開啟，等待用戶端存取")
                Toggle {
                    objectName: "mcpEnableButton"
                    Accessible.name: Theme.t("启用 MCP", "Enable MCP", "啟用 MCP")
                    checked: root.service.mcpEnabled || root.service.mcpStarting
                    enabled: root.service.mcpEnabled || (!root.service.busy && !root.service.mcpStarting)
                    onClicked: {
                        if (root.service.mcpEnabled) root.service.setMcpEnabled(false)
                        else root.service.startDesktopMcp(input.text.trim(), output.text.trim(), overwrite.checked)
                    }
                }
            }
            SettingsRow {
                label: Theme.t("连接客户端", "Connect a client", "連接用戶端")
                description: Theme.t("首次使用时，将配置添加到支持 stdio 的 MCP 客户端。以后开启即可重新连接。", "Add this configuration to your stdio MCP client once. Turn MCP on to reconnect.", "首次使用時，將設定加入支援 stdio 的 MCP 用戶端。之後開啟即可重新連線。")
                Tool {
                    objectName: "mcpCopyConfig"
                    text: Theme.t("复制连接配置", "Copy configuration", "複製連線設定")
                    onClicked: root.controller.copy(root.service.desktopMcpConfiguration())
                }
            }
            ExplainedText { objectName: "mcpServiceError"; Layout.fillWidth: true; kind: "error"; raw: root.service.error; dismissible: true; onDismissed: root.service.dismissError() }
        }
        SettingsGroup {
            Layout.fillWidth: true
            title: Theme.t("目录范围", "Folder access", "資料夾範圍")
            Label { text: Theme.t("只读输入目录", "Read-only input folder", "唯讀輸入資料夾"); color: Theme.text }
            RowLayout { Layout.fillWidth: true
                Field { id: input; objectName: "mcpInput"; Layout.fillWidth: true; enabled: root.editable; text: root.service.mcpEnabled ? root.service.scope.input_roots[0] : root.controller.preferences.mcpInputRoot || root.service.mcpDefaultRoot + "/Input"; onEditingFinished: root.controller.preference("mcpInputRoot", text.trim()); Accessible.name: Theme.t("只读输入目录", "Read-only input folder", "唯讀輸入資料夾") }
                Tool { text: Theme.t("选择…", "Choose…", "選擇…"); enabled: root.editable; onClicked: inputFolder.present() }
            }
            Label { text: Theme.t("输出目录", "Output folder", "輸出資料夾"); color: Theme.text }
            RowLayout { Layout.fillWidth: true
                Field { id: output; objectName: "mcpOutput"; Layout.fillWidth: true; enabled: root.editable; text: root.service.mcpEnabled ? root.service.scope.output_roots[0] : root.controller.preferences.mcpOutputRoot || root.service.mcpDefaultRoot + "/Output"; onEditingFinished: root.controller.preference("mcpOutputRoot", text.trim()); Accessible.name: Theme.t("输出目录", "Output folder", "輸出資料夾") }
                Tool { text: Theme.t("选择…", "Choose…", "選擇…"); enabled: root.editable; onClicked: outputFolder.present() }
            }
            OptionCheck { id: overwrite; checked: root.service.mcpEnabled && root.service.scope.allow_overwrite; enabled: root.editable; text: Theme.t("允许覆盖输出目录内的已有文件", "Allow overwriting existing output files", "允許覆寫輸出資料夾內的現有檔案") }
            Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted
                text: root.editable ? Theme.t("首次开启会自动创建默认目录。可在开启前更改访问范围。", "Default folders are created on first use. You can change the folders before enabling MCP.", "首次開啟會自動建立預設資料夾。可在開啟前變更存取範圍。")
                    : Theme.t("关闭 MCP 后可更改访问范围。", "Turn MCP off to change folder access.", "關閉 MCP 後可變更存取範圍。")
            }
        }
        Readout { Layout.fillWidth: true; label: Theme.t("当前构建的输出目标", "Output targets in this build", "目前組建的輸出目標"); value: (root.service.capabilities.write_targets || []).join(" / ").toUpperCase() }
    }
    FrostFolderDialog { parentWindow: root.Window.window; id: inputFolder; onAccepted: root.controller.preference("mcpInputRoot", root.controller.localFile(selectedFolder)) }
    FrostFolderDialog { parentWindow: root.Window.window; id: outputFolder; onAccepted: root.controller.preference("mcpOutputRoot", root.controller.localFile(selectedFolder)) }
}
