pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
ScrollView {
    id: root
    objectName: "mcpSettingsScroll"
    required property var controller
    readonly property var service: controller.imageJobs
    readonly property bool matches: service.configured && service.scope.input_roots[0] === input.text && service.scope.output_roots[0] === output.text && service.scope.allow_overwrite === overwrite.checked
    clip: true
    contentWidth: availableWidth
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() { if (root.visible) Qt.callLater(() => { const window = root.Window.window; if (window) Theme.revealIn(root, window.activeFocusItem) }) }
    }
    RowLayout {
        width: root.availableWidth; spacing: 22
        ColumnLayout {
            Layout.fillWidth: true; Layout.preferredWidth: 52; Layout.alignment: Qt.AlignTop; spacing: 16
            FormGroup { Layout.fillWidth: true; title: Theme.t("服务与连接", "Service and connection", "服務與連線")
                ColumnLayout { anchors.fill: parent; spacing: 10
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.accent; wrapMode: Text.WordWrap; text: root.service.mcpEnabled ? Theme.t("本地端点已启用", "Local endpoint enabled", "本機端點已啟用") : Theme.t("MCP 已关闭 · 默认不启动", "MCP disabled · Off by default", "MCP 已關閉 · 預設不啟動") }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.WordWrap; text: Theme.t("输入只读，输出仅限下方范围；GUI 与 MCP 使用同一任务服务。停止端点会断开客户端，已提交任务仍可在任务中心取消。关闭应用会中断尚未完成的工作，重新启用相同范围后可查看恢复状态。", "Inputs are read-only and outputs stay within the scope below. GUI and MCP share one job service. Stopping the endpoint disconnects clients; submitted jobs can still be cancelled in Tasks. Closing the app interrupts unfinished work; reapply the same scope to inspect recovery.", "輸入唯讀，輸出僅限下方範圍；GUI 與 MCP 使用同一工作服務。停止端點會中斷用戶端連線，已提交工作仍可在工作中心取消。關閉應用程式會中斷未完成工作，重新啟用相同範圍後可查看恢復狀態。") }
        Readout { Layout.fillWidth: true; visible: root.service.mcpEnabled; label: Theme.t("本地端点", "Local endpoint", "本機端點"); value: root.service.endpoint }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; visible: !root.service.lastClient.name; text: Theme.t("尚未观测到已握手客户端的工具请求；启用端点不等于客户端已连接。", "No tool request from an initialized client observed yet. An enabled endpoint does not mean a client is connected.", "尚未觀測到已握手用戶端的工具請求；啟用端點不代表用戶端已連線。") }
        Readout { Layout.fillWidth: true; visible: !!root.service.lastClient.name; label: Theme.t("最近已握手客户端 · 最近一次工具请求", "Last initialized client · Most recent tool request", "最近已握手用戶端 · 最近一次工具請求"); value: (root.service.lastClient.name || "") + " " + (root.service.lastClient.version || "") + "\nMCP " + (root.service.lastClient.protocol || "") + "\n" + (root.service.lastClient.observed_at || "") }
        Label { textFormat: Text.PlainText; text: Theme.t("已安装 MCP SDK 的 Python", "Python with MCP SDK installed", "已安裝 MCP SDK 的 Python"); color: Theme.text }
        RowLayout { Layout.fillWidth: true
            Field { id: python; objectName: "mcpPython"; Layout.fillWidth: true; text: root.controller.preferences.mcpPython || ""; onEditingFinished: root.controller.preference("mcpPython", text); Accessible.name: "Python" }
            Tool { text: Theme.t("选择…", "Choose…", "選擇…"); onClicked: pythonFile.open() }
        }
        Label { textFormat: Text.PlainText; text: Theme.t("已安装的 image_stdio.py 适配器", "Installed image_stdio.py adapter", "已安裝的 image_stdio.py 配接器"); color: Theme.text }
        RowLayout { Layout.fillWidth: true
            Field { id: adapter; objectName: "mcpAdapter"; Layout.fillWidth: true; text: root.controller.preferences.mcpAdapter || ""; onEditingFinished: root.controller.preference("mcpAdapter", text); Accessible.name: "image_stdio.py" }
            Tool { text: Theme.t("选择…", "Choose…", "選擇…"); onClicked: adapterFile.open() }
        }
        Tool { objectName: "mcpCopyConfig"; text: Theme.t("复制当前端点配置", "Copy current endpoint configuration", "複製目前端點設定"); enabled: root.service.mcpEnabled && !!python.text && !!adapter.text; onClicked: { const config = root.service.mcpConfiguration(python.text, adapter.text); if (config) root.controller.copy(config) } }
        Label { textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; text: Theme.t("仅复制配置，不修改客户端设置。当前端点随本次应用会话关闭；下一次启用后需复制新端点配置。", "Copies configuration without changing client settings. This endpoint belongs to the current app session; copy a new configuration after enabling it next time.", "僅複製設定，不修改用戶端設定。目前端點隨本次應用程式工作階段關閉；下次啟用後需複製新的端點設定。") }
        ExplainedText { objectName: "mcpServiceError"; Layout.fillWidth: true; kind: "error"; raw: root.service.error; dismissible: true; onDismissed: root.service.dismissError() }

                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.preferredWidth: 48; Layout.alignment: Qt.AlignTop; spacing: 16
            FormGroup { Layout.fillWidth: true; title: Theme.t("目录范围", "Folder scope", "資料夾範圍")
                ColumnLayout { anchors.fill: parent; spacing: 10
        Label { textFormat: Text.PlainText; text: Theme.t("只读输入目录", "Read-only input directory", "唯讀輸入目錄"); color: Theme.text }
        RowLayout { Layout.fillWidth: true
            Field { id: input; objectName: "mcpInput"; Layout.fillWidth: true; text: root.controller.preferences.mcpInputRoot || root.controller.directory; onEditingFinished: root.controller.preference("mcpInputRoot", text); Accessible.name: Theme.t("MCP 输入目录", "MCP input directory", "MCP 輸入目錄") }
            Tool { text: Theme.t("选择…", "Choose…", "選擇…"); onClicked: inputFolder.open() }
        }
        Label { textFormat: Text.PlainText; text: Theme.t("输出目录", "Output directory", "輸出目錄"); color: Theme.text }
        RowLayout { Layout.fillWidth: true
            Field { id: output; objectName: "mcpOutput"; Layout.fillWidth: true; text: root.controller.preferences.mcpOutputRoot || ""; onEditingFinished: root.controller.preference("mcpOutputRoot", text); Accessible.name: Theme.t("MCP 输出目录", "MCP output directory", "MCP 輸出目錄") }
            Tool { text: Theme.t("选择…", "Choose…", "選擇…"); onClicked: outputFolder.open() }
        }
        OptionCheck { Layout.fillWidth: true; id: overwrite; text: Theme.t("明确允许覆盖输出范围内的已有文件", "Explicitly allow overwriting files within the output scope", "明確允許覆寫輸出範圍內的現有檔案") }
        RowLayout {
            Tool { primary: true; objectName: "mcpApplyScope"; text: Theme.t("应用访问范围", "Apply access scope", "套用存取範圍"); enabled: !!input.text && !!output.text && !root.service.busy && !root.service.mcpEnabled; onClicked: root.service.configureScope({input_roots: [input.text], output_roots: [output.text], allow_overwrite: overwrite.checked}) }
            Tool { objectName: "mcpEnableButton"; text: root.service.mcpEnabled ? Theme.t("停止 MCP 端点", "Stop MCP endpoint", "停止 MCP 端點") : Theme.t("启用 MCP 端点", "Enable MCP endpoint", "啟用 MCP 端點"); enabled: root.service.mcpEnabled || (root.matches && !root.service.busy); onClicked: root.service.setMcpEnabled(!root.service.mcpEnabled) }
        }
                    SectionHeader { id: appliedDetails; Layout.fillWidth: true; text: Theme.t("实际生效范围", "Applied scope", "實際生效範圍") }
                    ColumnLayout { Layout.fillWidth: true; visible: appliedDetails.checked
        Readout { Layout.fillWidth: true; visible: root.service.configured; label: Theme.t("实际生效范围", "Applied scope", "實際生效範圍"); value: Theme.t("只读：", "Read only: ", "唯讀：") + (root.service.scope.input_roots || []).join("\n") + "\n" + Theme.t("写入：", "Write: ", "寫入：") + (root.service.scope.output_roots || []).join("\n") + "\n" + (root.service.scope.allow_overwrite ? Theme.t("允许覆盖输出", "Output overwrite allowed", "允許覆寫輸出") : Theme.t("不允许覆盖输出", "Output overwrite disabled", "不允許覆寫輸出")) }
                    }
                }
            }
            FormGroup { Layout.fillWidth: true; title: Theme.t("实际能力", "Available capabilities", "實際能力")
                ColumnLayout { anchors.fill: parent; spacing: 10
                    Readout { Layout.fillWidth: true; label: Theme.t("当前构建的输出目标", "Output targets in this build", "目前組建的輸出目標"); value: (root.service.capabilities.write_targets || []).join(" / ").toUpperCase() }
                    Label { Layout.fillWidth: true; color: Theme.muted; font.pixelSize: Theme.compactText; wrapMode: Text.WordWrap; text: Theme.t("每项输入仍需实际预检。GUI 与 MCP 共用范围、参数和任务。", "Each input still requires preflight. GUI and MCP share scope, settings and jobs.", "每項輸入仍須實際預檢。GUI 與 MCP 共用範圍、參數與工作。") }
                }
            }
        }
    }
    FolderDialog { id: inputFolder; onAccepted: { input.text = root.controller.localFile(selectedFolder); root.controller.preference("mcpInputRoot", input.text) } }
    FolderDialog { id: outputFolder; onAccepted: { output.text = root.controller.localFile(selectedFolder); root.controller.preference("mcpOutputRoot", output.text) } }
    FileDialog { id: pythonFile; onAccepted: { python.text = root.controller.localFile(selectedFile); root.controller.preference("mcpPython", python.text) } }
    FileDialog { id: adapterFile; onAccepted: { adapter.text = root.controller.localFile(selectedFile); root.controller.preference("mcpAdapter", adapter.text) } }
}
