# 4K 贴图打开与往返切换：2026-11-12

## 问题与修复

基线 `fafc3c3c8eba58bb39f0554551e586cb0c404b52` 对所有普通贴图创建
RGBA32F 上传缓冲。3840×2160 的 UInt8 UNorm TGA 因此需要 132,710,400
字节上传数据；逐像素解释、转换还占用约 95 ms。实际预览同时保留 planar
源通道、native encoded_blocks 和元数据，单图计费约 189.85 MiB，256 MiB
LRU 仅容纳一张。A/B/A 返回 A 时重新读取、解码、准备上传。

现在仅在所有已映射通道均为 UInt8 UNorm 时，直接交错原字节为 RGBA8。
没有颜色量化、gamma 转换、预乘/反预乘或 alpha 丢弃。缺失显示通道仍填
RGB=0、A=1，源通道是否存在及其 alpha 语义保持原样。像素检查器仍读取源数据。
数值型 UInt8、16/32 位整数、Half、Float32/64、HDR 和 SNorm 继续使用原来的
浮点解释路径。GIF 合成帧保持原浮点路径。

上传缓冲降至 33,177,600 字节；实际预览总计费降至约 94.92 MiB，同一缓存预算
可留两张本次样本。没有增大 256 MiB 缓存或 512 MiB 单图预算，没有绕过文件
读取租约、文件身份/更改时间校验和请求代次检查。QRhi 在尺寸或格式改变时重建
纹理，确保同尺寸的 RGBA8 与 RGBA32F 切换不复用错误资源。

## 测量范围

原始输入仅只读访问 `E:/RenderDocProject/RDC/ff16` 下九张 TGA（A、J、K、L、
M、N、O、P、Q）；均为 3840×2160、32 位、RLE TGA。未读取同目录 RDC 捕获文件，
未访问或写入 `B:/Model`。输入大小与 SHA-256 保存在 `original-inputs.json`，
测试前后核对一致。

探针使用 QCoreApplication 和生产 ImageSession，不创建窗口：

- `document_open_ms` 包含读取、源哈希与 native TGA 解码；不是纯磁盘读取时间。
- `decode_ms` 是 ImageDocument 转为公开 planar 源通道的时间。
- `prepare_upload_ms` 是生产上传数据准备函数的 CPU 时间。
- `ready_signal_ms` 是请求至 ImageSession 发布像素的时间，包含轮询；不是 GPU
  完成时间、Qt Quick 绘制时间或用户看到首帧的时间。
- `busy_signal_ms` 和事件循环心跳只证明 QCore 调度响应，不证明加载提示已画出。
- session-cold 指新建会话、应用缓存为空；未清空 OS 文件缓存，不能称为冷盘实验。
  阶段测量先于会话测量。A/B/A 使用 A.tga、J.tga、A.tga；接着重复三组 B/A，
  再顺序/倒序遍历九张图片，共 28 个请求。

首轮实测（Release；原始数据而非播放或模型指标）：

| 指标 | 基线 | 修复 |
|---|---:|---:|
| 单图上传准备，九张样本范围 | 94.67–96.35 ms | 10.78–12.77 ms |
| 新会话打开 A 的 CPU 就绪 | 266.96 ms | 187.05 ms |
| A/B/A 返回 A 的 CPU 就绪 | 285.06 ms，未命中 | 10.30 ms，命中 |
| 三组重复 B/A | 269.87–285.90 ms，均未命中 | 9.18–10.49 ms，均命中 |
| 单图上传字节 | 132,710,400 | 33,177,600 |
| 单图生产会话计费字节 | 199,068,110 | 99,535,310 |

另有相同顺序的基线/候选复测，完整数据在 `baseline-repeat.json` 与
`candidate-repeat.json`。四次测量的相同请求均核对源 SHA 和规范化上传像素 SHA，
28/28 一致。候选 A/B/A 与重复 B/A 均命中。倒序遍历命中末两张，超出预算的
旧图仍需解码，这是有界缓存的预期行为。

不要拿孤立阶段里的 `retained_bytes` 当作生产总内存：它只为微基准设置
planar+upload 计费。生产请求字段另含 native buffer 和元数据。进程内存字段
还受哈希校验临时缓冲与分配器保留影响，不用于声称 RSS 按相同比例降低。

## 回归与复现

新增 `formaview_image_session_tests`，由 Windows 专业图像测试配置构建。
`FORMAVIEW_PRO_TEST_ROOT` 应指向独立测试目录。示例：

```powershell
cmake --build out/msvc --config Release --target formaview_image_session_tests formaview
# PATH 应含 Qt bin 与已配置第三方库的运行目录。
out/msvc/tests/Release/formaview_image_session_tests.exe --regression E:/test-output/texture-fixtures E:/test-output/texture-regression.json
out/msvc/tests/Release/formaview_image_session_tests.exe --benchmark E:/RenderDocProject/RDC/ff16 E:/test-output/texture-benchmark.json
```

34 项覆盖全部 0–255 值、通道重映射/灰度/缺失 alpha、透明像素 RGB、alpha 元数据、
所有标量类型、HDR/负值/负零/无穷/NaN、非法编码与映射、截断缓冲、尺寸溢出、
预算边界、取消、4K A/B/A 命中、同大小同 mtime 文件改写失效、最新请求获胜、
错误恢复、有界 LRU 淘汰及 clear 后释放缓存。Release、MSVC ASan、Clang UBSan
trap 各运行 34 项；现有 QCore GIF 回归 23 项。逐次命令、退出码和日志存于证据目录。

本轮证据目录：
`E:/CodexProject/asset-preview-planning/development/verification/M11-texture-switch-20261112`。
`verify_final.py` 固定构建/执行顺序，保存 `*.command.json`；`final-verification.json`
是最终源码/输入/执行核对结果。基线探针保留为 `baseline-probe.exe`，取自优化前
仅提取原 float 准备函数的版本，不是用修复后的实现模拟旧路径。

## 失败记录与验证边界

失败日志均保留，不能以构建成功代替测试成功：

1. 新测试最初缺 Qt6::Qml target；现显式查找该依赖。
2. 格式化重排 Windows 头导致 psapi.h 编译失败；现将 windows.h 放在独立前置组。
3. 首次 GIF 运行缺插件搜索路径，20 项报 Unsupported image format。此前同一
   脚本打印的 Passed 来自先执行的 session 测试，并非 GIF 通过。设置
   `QT_PLUGIN_PATH=E:/Qt/6.12.0/msvc2022_64/plugins` 后单独重跑，23 项通过。
4. UBSan 关闭 FBX 的配置暴露既有 image_batch.cpp 条件编译缩进警告；仅该测试
   构建设置 `-Wno-error=misleading-indentation`，保留警告，未改产品代码或全局选项。
5. 本机 Clang UBSan runtime 使用静态 CRT，与依赖的动态 CRT 不兼容。改用
   `FORMAVIEW_UBSAN_TRAP=ON`；UB 会中止进程，但没有 runtime 的详细诊断。
6. LLD 合并 Qt manifest 生成错误命名空间，探针启动 WinError 14001。仅 UBSan
   构建设置 `CMAKE_LINKER_TYPE=MSVC`，用系统链接器生成有效 manifest；保留旧清单。
7. 对照汇总脚本最初漏计 native buffer，误要求倒序命中四张。现按生产计费字节
   和原 256 MiB 上限推导为两张，没有修改生产预算来迎合断言。

Release 主程序已编译；三套 session 测试是 CPU 级验证。第三方预编译库并非全部
受 sanitizer 插桩。未运行 GUI、Computer Use、QML preview、desktop_tests 或 Blender。
GPU 上传耗时、实际显卡颜色/alpha 显示、格式切换画面和屏幕首帧仍待后续图形验收。
首次打开仍有约百毫秒级读取/解码成本，不能宣称卡顿已在用户界面彻底消失。
本项不制作/清理压缩包、安装包或旧输出，也不安装、推送或发布。

## 审查

实现者二次检查了上传类型选择、浮点回退、边界/预算、取消与共享对象生命期。
统筹会话进行了独立只读审查，未发现阻断性逻辑问题，指出公开头应显式包含
`<cstddef>`；已补齐并重新构建、回归。独立审查不替代 GPU/图形复测。
