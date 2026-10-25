FormaView 功能运行目录 / Functional run directory / 功能執行資料夾
=====================================================================

这是功能验证用的运行目录，不是安装包（安装、升级、卸载由后续阶段提供）。
This is a functional run directory, not an installer.
這是功能驗證用的執行資料夾，不是安裝套件。

启动 / Start / 啟動
  双击 formaview.exe。无需修改 PATH，无需安装 Qt 或 Visual C++ 运行库（已随目录提供）。
  Run formaview.exe. No PATH change, Qt or Visual C++ redistributable install is needed.
  命令行 / Command line:
    formaview.exe --open <文件或目录>       打开文件或目录 / open a file or folder
    formaview.exe --capabilities             输出实际可用的图片与模型导出能力 / print real capabilities
    formaview.exe --smoke                    启动自检并退出 / startup self-check

目录内容 / Contents
  formaview.exe                  桌面程序 / desktop app
  formaview_image_service.exe    无窗口任务服务 / headless job service (GUI 与 MCP 共用同一套能力)
  formaview_image_cli.exe        服务命令行客户端 / service command-line client
  Qt6*.dll, platforms\, imageformats\, qml\ ...   Qt 6.12 运行库与插件 / Qt runtime
  OpenEXR / Imath / deflate / openjph / zstd DLL   图片格式库 / image format libraries
  mcp\                           MCP 适配器、依赖锁文件 / MCP adapter and lock file
  licenses\                      第三方许可与声明 / third-party licenses
  sha256.json                    全部文件的哈希清单 / file hash manifest

使用要点 / Usage notes
  - 打开目录或单文件后，在列表选择文件，右侧查看；菜单“文件 → 转换选定文件…”进入 设置 → 预检 → 任务。
  - 所有转换先预检；有损项必须勾选“已阅读并接受全部损失”才能提交。输出不会覆盖输入或其硬链接。
  - 模型/动画导出为 FBX：支持 PSK/PSKX(+PSA)、OBJ、glTF/GLB、FBX(多动作拆分)、PMX、VRM。
  - ABC 与视频不在本版范围内。 / ABC and video are not part of this release.

MCP（可选 / optional）
  MCP 默认关闭。需要 Python 3.12 与锁定依赖（需 uv）：
    uv venv <venv目录> --python 3.12
    uv pip sync --require-hashes mcp\requirements.lock --python <venv目录>\Scripts\python.exe
  然后在程序“设置 → 权限 / MCP”中：应用输入与输出范围，填入该 venv 的 python.exe 与 mcp\image_stdio.py 的绝对路径，
  启用端点，点击“复制当前端点配置”，把配置粘贴到 MCP 客户端。停止端点不会自动取消已提交的任务。
  MCP is off by default. Create a Python 3.12 venv, sync mcp\requirements.lock with hashes, apply an
  input/output scope in Settings, enable the endpoint and copy the generated client configuration.
