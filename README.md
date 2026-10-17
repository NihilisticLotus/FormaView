# FormaView

Windows x64，C++20，VS2022 v143 14.44.35207，Windows SDK 10.0.26100.0，CMake ≥3.25，Qt 6.8.3 `msvc2022_64`（Quick）。当前入口是工程运行宿主，可执行格式能力为空。

在 PowerShell 中运行：

```powershell
$env:FORMAVIEW_QT_ROOT = 'E:\CodexProject\asset-preview-planning\development\toolchain\Qt\6.8.3\msvc2022_64'
.\tools\dev.ps1 Configure
.\tools\dev.ps1 Build
.\tools\dev.ps1 Test
.\tools\dev.ps1 Run
.\tools\dev.ps1 Smoke
.\tools\dev.ps1 Lint
.\tools\dev.ps1 Bench
```

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

同时启用读写库后生成 `formaview_skeletal_probe` 测试工具，参数为输入、配对 PSK 或 `-`、已存在输出目录、报告路径。每片段分别生成纯骨架与配对网格 FBX，拒绝覆盖。`FORMAVIEW_TEST_ARTIFACT_ROOT` 指定外部测试输出目录后启用 FBX 数值测试。验证库尚未接入正式宿主或能力注册；不构成格式兼容性承诺。

对于使用 PSK 层级与参考姿势的 PSA，工具可追加两个参数：显式提供的 `.config` 路径（无配套文件时为 `-`）和 `reference`。此模式按唯一骨骼名称配对，纯骨架导出也使用 PSK 参考；保留 PSA 原始层级、姿势和键，不把记录差异当作文件损坏。`[RemoveTracks]` 的 `all`／`trans`／`rot` 使用 PSK 对应参考通道，配置内容与来源哈希保留。核心不自动扫描或访问相邻文件；默认严格记录检查仍可用于诊断。

PSA `BONENAMES` 中的全零四元数可作为缺失参考记录保留，`Bone::default_local_valid` 标记其不可用；动画键仍可读取和求值。默认姿势查询及无参考 PSK 的导出会明确拒绝，不能把占位记录当单位旋转或已验证绑定。PSK 绑定和实际动画键中的零四元数仍拒绝。测试工具可在 `reference` 后追加显式 FBX 读写预算字节数（默认 1 GiB，工具上限 16 GiB），同一预算传给写出及回读，预算调整不改变误差门槛。

`formaview_skeletal_inventory` 接受 UTF-8 TSV 清单、新 JSONL 报告路径和可选停止文件路径。清单每行是 `输入路径<TAB>参考PSK或-<TAB>config或-`；逐项记录哈希、结构、姿势、预算拒绝与错误，创建停止文件可取消批次。报告拒绝覆盖；每文件失败保留在报告内，调用方须检查 `accepted` 字段，不能只看进程退出码。

FBX 写出使用静态参考预旋转，并按世界矩阵误差细分超差的源帧区间；保留源关键时刻、真实秒和末尾保持。每个输出区间的三个内部时刻接受数值检查，超过精度、细分或预算限制时拒绝写出。文件帧率声明最细输出采样网格，原始帧率／帧数及实际输出采样率单独保存在 `FormaViewSourceFPS`／`FormaViewSourceFrames`／`FormaViewBakeFPS` 元数据中；不能把输出采样数当作原始 PSA 帧数。`FbxWriteResult` 返回细分、输出采样数和检查误差。验证工具还在独立的非网格时刻回读检查；这些实验性接口尚未接入正式导入流程。

Blender 4.5.x 的已验证导入流程需要仓库内 `tools/blender/formaview_import.py` 适配器，使关键帧位置使用 `fps/fps_base`；默认 FBX 导入尚未取得相同验收结论。在 Blender Python 控制台将该目录加入 `sys.path`，然后调用 `from formaview_import import import_verified_fbx; import_verified_fbx(r'C:\path\clip.fbx')`，或执行 `blender --python tools/blender/formaview_import.py -- C:\path\clip.fbx`。脚本检查 Blender 版本、导入器签名和显式来源元数据，临时修正仅在本次导入内生效；不改安装文件、不重新定时。独立契约测试：`py -3 tests/blender_import_tests.py`。

当前写出边界：名称只接受非空可打印 ASCII（读取保留原始名字字节，文件路径支持 Unicode）；负权重、morph 和未经解释的非均匀 key delta 拒绝写出。缩放键保留并求值，层级溢出／FBX 时间或曲线精度溢出会拒绝。读写预算与取消属于协作准入，FBX SDK 内部分配没有硬上限，ufbx 单次场景求值只能在调用前后检查取消。输出曲线的有限时刻误差检查不构成连续时间的数学误差上界。

专业数据验证目标可用 `-DFORMAVIEW_BUILD_PROFESSIONAL=ON` 启用。将锁定版本 OpenEXR、Imath、DirectXTex、Alembic 的外置安装前缀加入 `CMAKE_PREFIX_PATH`，并将 OpenEXR 及传递依赖 DLL 目录加入运行时 `PATH`。设置 `FORMAVIEW_PRO_TEST_ROOT` 为仓库外测试目录后运行 CTest。ASan 构建须使用同样启用 ASan 的 DirectXTex／Alembic 静态库，明确指定各自包目录及 `-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=Release`；已编译 OpenEXR DLL 内部不在 ASan 覆盖范围。

`formaview_professional_probe image|image-summary|abc|mesh 输入 外部输出目录` 输出数值核验材料；`image-summary` 仅输出通道哈希。启用 Qt 宿主时需要同版本 ShaderTools，另生成 `formaview_render_probe 外部输出目录 precision|image|mesh|abc [输入]` 技术窗口（ABC 渲染入口仅接收三角面）。渲染使用 Qt 6.8.3 QQuickRhiItem／D3D11／RGBA32F，与 Qt 私有 QRhi ABI 锁定；升级 Qt 必须重新验证。未接入产品能力注册。

图片与 ABC 打开／解码接口是阻塞工作线程 API，须经有界调度器调用。保留源样本与独立显示缓冲；DDS 的 sRGB 数值保持编码域，`apply_srgb` 只对线性显示输入编码，不能重复应用。EXR 暂限普通非子采样通道和 tiled level 0，ABC 暂限 Ogawa 变换／多边形及主 UV／法线；Deep EXR、HDF5 和其他 schema 明确拒绝。预算限制已知载荷，第三方解码器内部临时分配与不可中断调用不构成硬实时上界。渲染目标目前是技术验证组件，设备丢失／分配失败的产品级恢复仍需后续接入。
