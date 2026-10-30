# FormaView

冻结运行目录必须包含 `mcp/image_stdio.py`、`mcp/requirements.lock`、`mcp/requirements.txt` 和 `README-run.txt`。`tools/package_runtime.ps1` 收集这些源码文件并自动核查；手动制作候选也必须运行 `tools/verify_runtime.ps1 -RunDir <候选> -EvidenceDir <外部证据> -SourceRoot <源码仓库>`，检查清单之外的必要文件契约、逐文件来源、依赖闭包和启动。该验证支持 `sha256.json` 或候选 `payload-manifest.json`；`-ManifestOnly` 仅检查文件，不代表 MCP 运行通过。冻结前还须用锁定 Python 环境执行 `tests/runtime_mcp_tests.py`，通过包内适配器验证真实服务的 initialize/tools/list/call 和正常退出。不得用源码目录脚本替代候选里的脚本。Python/venv 是可选 MCP 的独立前置环境，不随桌面包提供，安装方法见 `README-run.txt`。

Windows x64，C++20，VS2022 v143 14.44.35207，Windows SDK 10.0.26100.0，CMake ≥3.25，Qt 6.12.0 `msvc2022_64`（Quick）。入口提供三语言（简中/繁中/英文）外壳、目录与单文件浏览、按需缩略图和任务状态。启用专业与图片转换目标后接入图片预览（读取 PNG/JPEG/BMP/TGA/TIFF/WebP/GIF/ICO/DDS/HDR/EXR）、批量转换（写出 PNG/JPEG/BMP/TGA/TIFF/WebP/DDS/HDR/EXR）和共用任务服务的 MCP；启用模型目标后接入 FBX/OBJ/glTF/GLB/PSK/PSKX 的模型预览（UV、完整源权重、骨架与绑定、LOD/碰撞）、FBX/glTF/PSA 动画播放与骨架比较，以及到 FBX 的模型/动画导出（PSK/PSKX(+PSA)、OBJ、glTF/GLB、FBX 多动作拆分、PMX、VRM；PMX/VRM 仅作转换输入，无专业预览）。转换前一律预检，损失与错误以三语言说明显示并保留可复制的技术码。`--capabilities` 查询实际编入且处理器可用的能力。ABC 与视频不在本版范围内。功能运行目录见 `tools/package_runtime.ps1` 与 `tools/verify_runtime.ps1`（不是安装包）。

可使用 `formaview.exe --open 文件或目录`，或在窗口中按 `Ctrl+O` 打开文件、`Ctrl+L` 编辑目录，方向键连续浏览、Ctrl／Shift 多选。图片视口支持通道、独立 Alpha、曝光、平铺、像素读数，`F` 适应窗口、`Z` 一个源像素对应一个屏幕物理像素。其他格式的文件读取检查不代表解析成功。转换使用源内容及显式设置，不继承视口状态；先预检、审阅损失再提交。MCP 默认关闭，在设置中应用输入/输出范围后启用，填入独立 Python 环境和适配器路径并复制配置给客户端；停止端点不会自动取消已提交任务。

产品定位为只读资产预览，不提供资产编辑、保存或保存回原文件的操作。通道、曝光、平铺等预览调整只改变显示；“保存设置”只保存软件偏好。批量转换是独立导出流程，输出不得覆盖本次输入资产或其硬链接，覆盖策略仅用于既有输出文件。

配置时指定仓库外的 `FORMAVIEW_UI_TEST_ROOT` 可启用桌面集成测试。测试生成约一万项临时目录并保留证据；原生输入测量需要交互式 Windows 桌面，会临时将测试窗口置于前台。`--smoke --metrics 外部JSON路径` 记录外壳启动与资源观测，计时器间隔不等同输入延迟。

在 PowerShell 中运行：

```powershell
$env:FORMAVIEW_QT_ROOT = 'E:\Qt\6.12.0\msvc2022_64'
.\tools\dev.ps1 Configure
.\tools\dev.ps1 Build
.\tools\dev.ps1 Test
.\tools\dev.ps1 Run
.\tools\dev.ps1 Smoke
.\tools\dev.ps1 Lint
.\tools\dev.ps1 Bench
```

QML Preview MCP 使用独立外置构建目录，配置 `-DFORMAVIEW_QML_PREVIEW=ON` 并构建 `RelWithDebInfo`（或使用匹配 Debug 依赖的 Debug）。此选项默认关闭，Release 始终不编入 QML 调试服务。预览使用可执行文件旁的 `preview-state` 保存隔离设置与任务；向其中的 `capture.request` 写入简单文件名，可保存该窗口的 PNG 和尺寸/DPR JSON，供本地界面核查。

需要隔离验证 Qt 预览插件修正时，可设置 `FORMAVIEW_QML_PREVIEW_PLUGIN_DIR` 为包含 `qmltooling/qmldbg_preview.dll` 的外置目录。仅开发预览配置使用该路径，配置时锁定 DLL 的 SHA-256，启动时再次核对；文件变化须重新配置。此选项不覆盖 Qt 安装，Release 不加载该覆盖目录。

本地诊断可使用 `formaview --trace <外置JSON路径>`，记录实际窗口尺寸、原生输入与同步帧、模型首帧和进程资源；该模式单独保存设置和任务。原始消息可能没有画面变化，相机延迟只统计实际触发相机变化的输入；帧交换信号不等于显示器呈现时间。

核心可独立于 Qt 构建（CMake／CTest 需在 PATH）：

```powershell
cmake --preset windows-msvc -DFORMAVIEW_BUILD_HOST=OFF
cmake --build --preset release
ctest --preset release
cmake --preset windows-asan
cmake --build --preset asan
ctest --preset asan
```

可选骨骼验证目标：原生 ActorX 核心无需 Qt 或 SDK。设置 `FORMAVIEW_UFBX_ROOT` 为锁定版本的外置 ufbx 源码目录，可构建 FBX 读取库；设置 `FORMAVIEW_BUILD_FBX_WRITER=ON` 与 `FORMAVIEW_FBXSDK_ROOT` 可构建本机 FBX SDK 写出验证库。SDK 由使用者自行取得合法安装，不随源码提供。版本和校验值见 `cmake/dependencies.lock.json`；第三方通知在 `third_party/`。

同时启用读写库后生成 `formaview_skeletal_probe` 测试工具，参数为输入、配对 PSK 或 `-`、已存在输出目录、报告路径。每片段分别生成纯骨架与配对网格 FBX，拒绝覆盖。`FORMAVIEW_TEST_ARTIFACT_ROOT` 指定外部测试输出目录后启用 FBX 数值测试。该探针是独立验证入口；产品内导出由共用任务服务（能力 `model.export_fbx`）提供，探针结果不构成格式兼容性承诺。

对于使用 PSK 层级与参考姿势的 PSA，工具可追加两个参数：显式提供的 `.config` 路径（无配套文件时为 `-`）和 `reference`。此模式按唯一骨骼名称配对，纯骨架导出也使用 PSK 参考；保留 PSA 原始层级、姿势和键，不把记录差异当作文件损坏。`[RemoveTracks]` 的 `all`／`trans`／`rot` 使用 PSK 对应参考通道，配置内容与来源哈希保留。核心不自动扫描或访问相邻文件；默认严格记录检查仍可用于诊断。

PSA `BONENAMES` 中的全零四元数可作为缺失参考记录保留，`Bone::default_local_valid` 标记其不可用；动画键仍可读取和求值。默认姿势查询及无参考 PSK 的导出会明确拒绝，不能把占位记录当单位旋转或已验证绑定。PSK 绑定和实际动画键中的零四元数仍拒绝。测试工具可在 `reference` 后追加显式 FBX 读写预算字节数（默认 1 GiB，工具上限 16 GiB），同一预算传给写出及回读，预算调整不改变误差门槛。

`formaview_skeletal_inventory` 接受 UTF-8 TSV 清单、新 JSONL 报告路径和可选停止文件路径。清单每行是 `输入路径<TAB>参考PSK或-<TAB>config或-`；逐项记录哈希、结构、姿势、预算拒绝与错误，创建停止文件可取消批次。报告拒绝覆盖；每文件失败保留在报告内，调用方须检查 `accepted` 字段，不能只看进程退出码。

FBX 写出使用静态参考预旋转，并按世界矩阵误差细分超差的源帧区间；保留源关键时刻、真实秒和末尾保持。每个输出区间的三个内部时刻接受数值检查，超过精度、细分或预算限制时拒绝写出。文件帧率声明最细输出采样网格，原始帧率／帧数及实际输出采样率单独保存在 `FormaViewSourceFPS`／`FormaViewSourceFrames`／`FormaViewBakeFPS` 元数据中；不能把输出采样数当作原始 PSA 帧数。`FbxWriteResult` 返回细分、输出采样数和检查误差。验证工具还在独立的非网格时刻回读检查；产品导出通过同一写出库接入共用任务服务，并在预检中显示这些参数。

Blender 4.5.x 的已验证导入流程需要仓库内 `tools/blender/formaview_import.py` 适配器，使关键帧位置使用 `fps/fps_base`；默认 FBX 导入尚未取得相同验收结论。在 Blender Python 控制台将该目录加入 `sys.path`，然后调用 `from formaview_import import import_verified_fbx; import_verified_fbx(r'C:\path\clip.fbx')`，或执行 `blender --python tools/blender/formaview_import.py -- C:\path\clip.fbx`。脚本检查 Blender 版本、导入器签名和显式来源元数据，临时修正仅在本次导入内生效；不改安装文件、不重新定时。独立契约测试：`py -3 tests/blender_import_tests.py`。

当前写出边界：名称只接受非空可打印 ASCII（读取保留原始名字字节，文件路径支持 Unicode）；负权重、morph 和未经解释的非均匀 key delta 拒绝写出。缩放键保留并求值，层级溢出／FBX 时间或曲线精度溢出会拒绝。读写预算与取消属于协作准入，FBX SDK 内部分配没有硬上限，ufbx 单次场景求值只能在调用前后检查取消。输出曲线的有限时刻误差检查不构成连续时间的数学误差上界。

专业数据验证目标可用 `-DFORMAVIEW_BUILD_PROFESSIONAL=ON` 启用。将锁定版本 OpenEXR、Imath、DirectXTex、Alembic 的外置安装前缀加入 `CMAKE_PREFIX_PATH`，并将 OpenEXR 及传递依赖 DLL 目录加入运行时 `PATH`。设置 `FORMAVIEW_PRO_TEST_ROOT` 为仓库外测试目录后运行 CTest。ASan 构建须使用同样启用 ASan 的 DirectXTex／Alembic 静态库，明确指定各自包目录及 `-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=Release`；已编译 OpenEXR DLL 内部不在 ASan 覆盖范围。

`formaview_professional_probe image|image-summary|abc|mesh 输入 外部输出目录` 输出数值核验材料；`image-summary` 仅输出通道哈希。启用 Qt 宿主时需要同版本 ShaderTools，另生成 `formaview_render_probe 外部输出目录 precision|image|mesh|abc [输入]` 技术窗口（ABC 渲染入口仅接收三角面）。渲染使用 Qt 6.8.3 QQuickRhiItem／D3D11／RGBA32F，与 Qt 私有 QRhi ABI 锁定；升级 Qt 必须重新验证。探针是独立验证入口；产品图片能力由共用图片服务报告。

图片与 ABC 打开／解码接口是阻塞工作线程 API，须经有界调度器调用。保留源样本与独立显示缓冲；DDS 的 sRGB 数值保持编码域，`apply_srgb` 只对线性显示输入编码，不能重复应用。EXR 暂限普通非子采样通道和 tiled level 0，ABC 暂限 Ogawa 变换／多边形及主 UV／法线；Deep EXR、HDF5 和其他 schema 明确拒绝。预算限制已知载荷，第三方解码器内部临时分配与不可中断调用不构成硬实时上界。产品图片视口报告 GPU 分配失败；完整设备丢失恢复仍需专测。ABC 渲染仅为历史技术验证组件，不属于本版产品能力。

CPU图片转换库可另启用 `FORMAVIEW_BUILD_RASTER=ON`，需要锁定版本libtiff4.7.2、libwebp1.6.0、zlib1.3.2的外置静态安装前缀及QtGui；同时启用professional后构建 `FormaView::ImageConversion`。zlib使用config包的static组件；TIFF启用Deflate、LZW、PackBits，WebP包含demux/mux。版本、源包摘要和依赖许可分别见锁文件与third_party。ASan须使用重新instrument的三个静态库，Qt自身的二进制image handlers不在instrument范围内。

`ImageDocument`／`prepare_image_conversion`是无窗口worker API，按显式子资源、通道、Alpha、数值和元数据策略生成字节及损失报告。Windows `InputLease`／`OutputTransaction`提供本地路径与重解析点检查、输入锁定和临时输出提交；`ImageBatchService`复用这些接口实现后台预检、摘要确认、幂等提交、分页、逐项失败、取消、重试预检和日志恢复。正式图片界面、MCP和无窗口服务共用这些接口与原生能力目录。

同时启用图片读写库时生成无窗口 `formaview_image_service --scope 显式JSON文件 --journal 专用已存在目录 --endpoint 本用户管道名`，依赖同版本QtNetwork。scope含 `input_roots`、`output_roots`、`allow_overwrite`、`policy_revision`；只接受本地绝对路径，拒绝重解析点。journal必须是专用空目录或已有服务目录，服务将其ACL限制为当前用户，并独占拥有；客户端断连不会取消已提交任务。默认单个批处理worker、16排队任务、每计划64项、保留128条计划/任务，达到上限明确拒绝；不自动丢弃幂等记录。检查与批任务共用图像准入，同一时刻只执行一个图像操作，前台检查优先获得下一个文件操作边界；状态/取消不等待该准入。输入和解码载荷默认各512MiB，调度保留3GiB估计空间，第三方内部内存不构成硬上限。预检为重命名分配并显示确定性路径，避开同批其他目标；相同目标仅允许通过显式重命名分开，多个非重命名项仍须使用不同目标。实际路径进入审批摘要，执行不滚动后缀；批准后被占用需重新预检。

MCP技术入口 `tools/mcp/image_stdio.py` 使用官方SDK1.30.0；Python3.12/Windows依赖及hash在 `tools/mcp/requirements.lock`。在独立环境用 `uv pip sync --require-hashes tools/mcp/requirements.lock` 安装，再显式设置 `FORMAVIEW_IMAGE_ENDPOINT` 为已启动服务名。适配器直接连接本用户管道，查询/取消与耗时请求分别限流，不为每次请求启动进程。入口提供原生能力查询、目录发现/分页、inspect、create/get plan、submit/get/list job、cancel、retry plan；所有目标写出必须先审阅完整分页预检并提交摘要，有损项还需明确ack。目录发现只记录候选文件成员，排除并报告重解析点，上限4096图片/512目录/100000访问项；发现记录也计入128条保留上限。此入口不会修改用户MCP配置或自动启动服务。单文件验证模式需另设 `FORMAVIEW_IMAGE_CLI` 为client exe绝对路径、`FORMAVIEW_IMAGE_SCOPE` 为scope文件，并省略endpoint；此模式无持久任务承诺。

恢复保留已记录的逐项结果；未完成任务不自动复跑。临时文件从创建时启用系统自动删除，只有提交意图日志写入后才以FileDispositionInfoEx解除该状态并原子改名；文件系统不支持时明确拒绝，不退回无保护提交。启动时只将文件身份与内容摘要均吻合的输出核对为成功，只清理身份吻合的已记录临时文件。恢复时不推断或通配删除未知文件。断电、文件系统故障、其他卷类型及完整部署仍待专测。GUI与MCP共用这些接口；实际文件兼容性仍需逐项预检。
