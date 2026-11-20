# v0.1 模型预览显示模式

用于材质缺失或材质本身为黑色时检查模型。模型预览工具栏使用一个显示模式选择器，包含四个互斥选项；原独立线框按钮合并到此处，不改变版本号。

| 模式 | 行为 |
| --- | --- |
| 默认显示 | 保留原材质、贴图、透明度及既有灯光行为。 |
| 棋盘格材质 | 忽略原材质及透明度，使用带光照的程序化灰色棋盘格；有 UV 时按 UV 显示，无 UV 时根据表面方向投影到模型空间。采用导数过滤降低远处闪烁。 |
| 顶点色显示 | 忽略材质、贴图、光照和透明度，显示第一组源顶点 RGB；未提供该数据时使用中性灰，不将材质色冒充顶点色。 |
| 线框显示 | 沿用现有 GPU 线框实现，并使用可见的浅色、不透明线条。 |

FBX 从 `colour/0` 读取第一组角点色，glTF 从 `COLOR_0` 读取，ActorX 从 wedge 颜色读取。颜色以独立属性传入静态、GPU 骨骼和流式几何渲染；保留原默认着色数据。当前未解码源颜色的适配器显示中性灰。顶点 alpha 不用于此 RGB 检查模式。

切换显示模式仅更新渲染状态，不重建模型、不移动相机、不修改原始文件及导出内容。打开新文档恢复默认。UV 视图、源权重热图、地面网格和坐标轴保持各自检查语义；线框仍可用于三维权重视图。棋盘格复用当前三维材质所选 UV 与其变换，不替代独立 UV 检查视图。

验证入口：

- `formaview_model_render_tests OUTPUT_JSON`：实际 D3D11 像素回读，包括黑色/全透明材质下的独立 RGB、UV/空间棋盘格、不透明覆盖、UV 隔离及原材质回归。
- `formaview_model_state_tests FIXTURES UFBX_FIXTURES BLENDER_FIXTURES`：源顶点 RGB 与材质乘色分离、原模型状态回归。
- `FORMAVIEW_MODEL_PREVIEW_SAMPLE` 指向只读问题 FBX，设置隔离 `FORMAVIEW_UI_TEST_ROOT`，运行 `formaview_desktop_tests modelPreviewDisplayModes`：四模式控件、相机/几何不变、文档复位及截图。
- `formaview_model_gpu_tests SAMPLE OUTPUT_JSON 1 numeric`：既有骨骼 GPU 与 CPU 数值对照，覆盖新增顶点布局。

原始问题样本：`SK_Claymore_PhantomCicada.FBX`，27,733 点、31,715 三角形。样本及测试截图、日志仅保存在忽略的本地 `development/` 中。

三维视口的 N 和 T 可独立开启。蓝线为法线，红线优先显示源切线，缺失时用当前 UV 集计算。状态行标明切线来源。网格按三角形角点取样，最多选取 512 个三角形；动画随当前姿态更新。
