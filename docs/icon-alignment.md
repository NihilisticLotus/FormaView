# FormaView 应用与文件类型图标

## 设计稿与主资源

应用图标采用用户最终确认的蓝橙立体标志，不添加四角边框或不透明底色。透明 PNG 保留原稿的透视、圆角、渐变高光和阴影；主图为 RGBA，四角 Alpha 为 0。主图文件为 `src/host/icons/formaview.png`，尺寸为 1254 × 1254。

## 应用接入与生成

- 标题栏/系统窗口图标由 `src/host/main.cpp` 从 Qt 资源 `:/formaview/icons/formaview.png` 加载。
- 左侧导航品牌图标由 `src/host/NavigationRail.qml` 加载同一 PNG 资源，避免窗口图标与应用内品牌标志分叉。
- `src/host/CMakeLists.txt` 将 PNG 纳入 Qt 资源；Windows 可执行文件和安装器使用多尺寸 ICO。
- `src/host/icons/formaview.ico` 是 Windows 图标资源，包含 16、20、24、32、40、48、64、128 和 256px 九种尺寸，每层保留 Alpha。
- `tools/generate_app_icon.py` 以透明 PNG 为唯一主图生成 ICO，不再绘制或维护第二份独立图形。重新生成命令：`py -3 tools/generate_app_icon.py`。

## 文件类型图标（v0.2 开发版，2026-11-23）

采用用户已确认的三类蓝橙立体图标；应用本身的品牌图标保持不变。源稿来自当日独立设计会话，正式资源已完整纳入 `src/host/icons/file-types/`，构建不读取仓库外规划目录。

| 类别 | 图形 | 分类依据 |
|---|---|---|
| 视频 | 大播放三角 | 安装器 FORMAT_GROUPS 的“视频”，包括 FLV |
| 图片 | 山景与太阳 | “图片”组 |
| 模型 | 独立立方体 | “模型与缓存”“动画与骨架”，包括 ABC、PSA |

安装器仍只注册现有支持的扩展名：包含 libmpv 时为 36 种，不含时为 25 种。三类 ICO 命名为 `formaview-file-<image|video|model>-icon-<SHA-256>.ico`，`DefaultIcon` 指向对应类别的文件。类别或图形变化均会改变路径，使 Explorer 不再沿用旧的共享图标缓存键。升级仅在 registry 值仍与 FormaView 所有权账本中的旧值一致时更新；用户或其他程序的外部改写保留。沿用已有默认程序登记边界，不写 UserChoice；注册完成后仍发送 Shell 关联变更通知。所有类别图标纳入安装载荷和 owned-files 清单，构建记录的 `fileIcons` 保存各类别文件名及哈希。

软件内的 `FileTypeIcon.qml` 使用同一资源集，覆盖检查页文件列表/平铺/缩略图占位、素材浏览网格与列表、分类侧栏、文件标签和预览标题。目录保留文件夹标识，未知类型回退为通用文件图标。路径分类复用 Desktop 的既有格式判断；转换专用文件仍使用模型图标，这不会新增其专业预览或 Windows 关联支持。真实缩略图就绪时隐藏占位图；失败或尚无缩略图时才显示类别标识。播放/暂停等动作按钮继续使用原动作图标。

PNG 主图为 1254 × 1254 RGBA；派生 PNG 包含 16/20/24/32/40/48/64/128/256/512px，ICO 包含截至 256px 的九层 32-bit Alpha 图像。软件内按逻辑尺寸乘屏幕 DPR，选择不小于需求的派生尺寸；不对类别图标着色。Qt 资源只嵌入派生 PNG，不嵌入主图或 ICO。主图及生成脚本用于可重建源码，ICO 用于安装器。

Windows 重建命令：

```powershell
& ./src/host/icons/file-types/export-icons.ps1
py -3 -B tests/file_type_icon_tests.py
py -3 -B tests/installer_icon_cache_tests.py
```

脚本从相邻主图重建 PNG、ICO 和 manifest.json。安装器名称直接取当前 ICO 内容哈希。图标层、Alpha 和哈希测试以及编译不等同于 Explorer 实际安装验收：隔离环境仍需检查旧共享图标 → 新三类图标的升级、外部修改保留、不同 DPI 和卸载。此次开发不修改本机注册表，不覆盖已安装版本。

## 回归检查

运行 `py -3 -B tests/app_icon_tests.py`。测试检查主 PNG 为 RGBA、存在透明背景、主体在画布内居中且包含蓝橙颜色；并检查 ICO 含有规定的九种尺寸、透明角点和可见主体。2026-11-15 的本地验收还用 Windows 资源只读方式核对了应用与安装包内的九层 `RT_ICON` 图像哈希；该检查只读取 PE 资源，没有执行安装器入口。系统任务栏、资源管理器、快捷方式缓存，以及多屏 DPI 下的实际显示仍需在用户环境人工确认。
