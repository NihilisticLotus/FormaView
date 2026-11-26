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
  导出：get_image_capabilities(source_extension) 可按输入后缀查询输出格式。plan_image_conversion
  按输出后缀和实际通道准备图片转换。plan_fbx_export 支持源坐标或目标软件预设、单位和动画片段。
  plan_image_batch 接收 1–4096 个选中的图片路径、输出目录与扩展名，逐项匹配通道，保留原主名。
  plan_image_channels 按实际通道名拆分源图。选择 A 可导出透明度遮罩，选择 EXR 可保留浮点深度和源通道名。name_template 可用 {channel}；每份计划确认后才写文件。
  两种图片计划都支持 settings，可指定缩小比例、通道映射、合成背景和目标格式的编码参数。
  直通 Alpha 的 sRGB 素材可用 composite、background_linear 与 srgb_to_srgb 在线性空间合成，再输出为 sRGB。
  plan_gif_frames 按从 0 开始的帧号将 GIF 连续帧合成为 PNG，每份计划最多 4096 帧。可分段选择，文件名保留原帧号。
  plan_image_subresources 将同一图片的多个子资源按源索引分别导出。可用 indices 选取离散项，
  或用 match 按 DDS 的 mip、array_layer、cube_face、depth_slice 或 EXR 的 part_name 筛选。
  plan_fbx_batch 可将多个模型按相同坐标系和单位分别导出为 FBX，默认保留内嵌动画。
  多份 PSA 可用 reference 共用一个 PSK/PSKX，分别应用各自同名的 .config。
  plan_fbx_export 和 plan_fbx_clips 可用 companion 指定轨道配置。配对默认严格核对骨骼记录，
  若 PSA 缺少有效参考姿态，可显式选择 pairing_policy="reference-skeleton" 使用匹配的模型骨架。
  用 discover_export_sources 在授权目录查找图片或模型，get_image_discovery 分页返回路径和类别。
  两个模型或图片输出名相同时，可用 conflict="rename" 在预检中分配独立文件名。
  批量图片和 FBX、GIF 拆帧、子资源及逐片段导出可用 name_template="{index}-{name}.{ext}" 自定义文件名。
  index 是本批输出顺序，从 1 起算。逐片段 FBX 可用 {clip} 写入动作名。省略模板使用各工具原有命名。
  每条请求最多 1 MiB。超过 64 项仍只需确认整份计划一次，按 next_offset 翻页即可检查全部目标。
  用 get_image_plan 核对计划中的目标和损失后提交 submit_image_job，再用 get_image_job 查看实际输出。
  get_image_plan 的 summary_only=true 可一次查看整份计划的损失、错误及预计覆盖、跳过、改名和新建数量。用各类别的 first_index 作为分页 offset 可定位样例。
  set_export_job_paused 可在文件之间暂停或恢复同一个任务。get_image_job 的 summary_only=true 返回整份任务的状态与计数。
  在“软件设置 → MCP 与权限”打开开关。输入/输出默认使用文档目录下的 FormaView/MCP/Input 与 Output；
  也可在开启前更改目录。首次点击“复制连接配置”，把它添加到支持 stdio 的客户端；之后可重复开关。
  The Windows package includes a standalone stdio adapter. Enable MCP in Settings and add the copied
  configuration to your client once. The same configuration reconnects after restarting the desktop.
  停止共享后，已提交的任务可在任务中心管理。

  无窗口工作流：将客户端 command 指向本包 mcp\formaview-mcp.exe，不传 --connect。
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
