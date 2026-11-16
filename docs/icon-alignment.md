# FormaView 应用图标

## 设计稿与主资源

应用图标采用用户最终确认的蓝橙立体标志，不添加四角边框或不透明底色。透明 PNG 保留原稿的透视、圆角、渐变高光和阴影；主图为 RGBA，四角 Alpha 为 0。主图文件为 `src/host/icons/formaview.png`，尺寸为 1254 × 1254。

## 应用接入与生成

- 标题栏/系统窗口图标由 `src/host/main.cpp` 从 Qt 资源 `:/formaview/icons/formaview.png` 加载。
- 左侧导航品牌图标由 `src/host/NavigationRail.qml` 加载同一 PNG 资源，避免窗口图标与应用内品牌标志分叉。
- `src/host/CMakeLists.txt` 将 PNG 纳入 Qt 资源；Windows 可执行文件和安装器使用多尺寸 ICO。
- `src/host/icons/formaview.ico` 是 Windows 图标资源，包含 16、20、24、32、40、48、64、128 和 256px 九种尺寸，每层保留 Alpha。
- `tools/generate_app_icon.py` 以透明 PNG 为唯一主图生成 ICO，不再绘制或维护第二份独立图形。重新生成命令：`py -3 tools/generate_app_icon.py`。

Windows 文件类型关联使用安装器从 ICO 内容哈希生成的独立文件名（`formaview-file-icon-<SHA-256>.ico`），`DefaultIcon` 注册指向该文件，而不是固定路径的 `formaview.exe`。当图标更新时文件名随内容变化，避免 Explorer 将新图标误认为同一路径下的旧缓存图像。升级仅在当前 registry 值仍与 FormaView 所有权记录一致时更新；若该值已被用户或其他程序改写，则保留外部更改。注册流程完成后发送 Shell 文件关联变更通知。

## 回归检查

运行 `py -3 -B tests/app_icon_tests.py`。测试检查主 PNG 为 RGBA、存在透明背景、主体在画布内居中且包含蓝橙颜色；并检查 ICO 含有规定的九种尺寸、透明角点和可见主体。2026-11-15 的本地验收还用 Windows 资源只读方式核对了应用与安装包内的九层 `RT_ICON` 图像哈希；该检查只读取 PE 资源，没有执行安装器入口。系统任务栏、资源管理器、快捷方式缓存，以及多屏 DPI 下的实际显示仍需在用户环境人工确认。
