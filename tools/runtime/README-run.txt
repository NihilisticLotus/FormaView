FormaView 功能运行目录 / Functional run directory / 功能執行資料夾
=====================================================================

这是功能验证用的运行目录，不是安装包（安装、升级、卸载由后续阶段提供）。
This is a functional run directory, not an installer.
這是功能驗證用的執行資料夾，不是安裝套件。

启动 / Start / 啟動
  双击 formaview.exe。无需修改 PATH，无需安装 Qt 或 Visual C++ 运行库（已随目录提供）。
  Run formaview.exe. No PATH change, Qt or Visual C++ redistributable install is needed.
  开发验证版与安装版分别保存各自的任务历史。 / Development and installed builds store separate task histories.
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
  - ABC：支持 Ogawa 多边形几何缓存预览、采样时间轴，以及当前采样导出为静态 FBX。
  - ABC: Ogawa polygon cache preview and current-sample static FBX export.

MCP（可选 / optional）
  MCP 默认关闭。需要 Python 3.12 与锁定依赖（需 uv）：
  以下命令在本运行目录中执行；venv 创建在你选择的独立可写目录，不放入安装目录。
    uv venv <venv目录> --python 3.12
    uv pip sync --require-hashes mcp\requirements.lock --python <venv目录>\Scripts\python.exe
  然后在程序“设置 → 权限 / MCP”中：应用输入与输出范围，填入该 venv 的 python.exe 与 mcp\image_stdio.py 的绝对路径，
  启用端点，点击“复制当前端点配置”，把配置粘贴到 MCP 客户端。停止端点不会自动取消已提交的任务。
  MCP is off by default. Create a Python 3.12 venv, sync mcp\requirements.lock with hashes, apply an
  input/output scope in Settings, enable the endpoint and copy the generated client configuration.
  使用本目录随附的 mcp\image_stdio.py，不要引用源码树或其他旧版本目录中的脚本。
  Python 虚拟环境不随包提供；仅使用桌面预览/转换不需要 Python，也不需要启用 MCP。

  无窗口工作流：将客户端 command 指向上述 python.exe，args 指向本包 mcp\image_stdio.py。
  在该客户端配置的 env 中设置 FORMAVIEW_DESKTOP_SCOPE 为一个 UTF-8 JSON 文件的绝对路径。例如：
    {"input_roots":["D:/Media/Videos","D:/Media/Textures"],"output_roots":["D:/FormaView输出"],"allow_overwrite":false}
  目录须已存在。适配器随连接启动同一包的 formaview.exe，断开后退出。后台视频默认暂停，使用无声输出。
  open_asset 返回 asset_id。用 control_video 播放、暂停、定位、调速和调音量；用 step_video_frame 单帧前进或后退。
  capture_video_frame 将当前视频帧写入授权目录的新 PNG。read_texture_pixel(path,x,y,subresource) 可直接读取贴图源通道。
  video_playlist 可分页查看当前目录的视频，按索引选集或切换上一部、下一部；切换后使用返回的新 asset_id。
  list_video_tracks 枚举音轨与字幕，select_video_track 切换对应类型的 ID（0 为关闭）。
  load_video_subtitle 在输入授权范围内加载 SRT、ASS、SSA 或 VTT。video_ab_loop 按绝对秒数设置或清除 A–B。
  set_video_filter 调整亮度、对比度、饱和度（整数 -100 到 100）或锐化（0 到 2），传 0 可还原该项。
  若要同步控制桌面正在显示的播放器，用设置页生成的配置。也可以用 formaview.exe --mcp-scope <范围文件绝对路径>
  启动有窗口的主程序，从标准输出读取 endpoint，给适配器设置 FORMAVIEW_IMAGE_ENDPOINT 和 FORMAVIEW_DESKTOP=1。
  同一个客户端配置选择一种连接方式。读播放器时间只反映当前播放器状态，显示器呈现及实际音响输出需要独立验证。
窗口 MCP：open_asset(path, window=True) 按需启动可见播放器，普通 open_asset 继续在后台执行。
get_window_state 返回显示器 ID、窗口模式、置顶与逻辑像素边界。control_player_window 支持 normal、fullscreen、mini、move、resize。

当前账户从安装版 v0.2.0 降回官方 v0.1.0 时，在本目录的 PowerShell 中运行：
  .\rollback-to-0.1.0.ps1 -Installer "D:\下载\FormaView-0.1.0-Windows-x64-Setup.exe"
脚本使用原安装目录和当前文件类型，检查旧安装器的实际登记结果，需要时自动运行一次修复。
视频依赖的版本、构建来源和许可见 licenses\media-runtime-NOTICE.txt 及 licenses\media-builds.json。
