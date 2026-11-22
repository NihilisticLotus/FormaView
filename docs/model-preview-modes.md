# 模型预览与显示模式

v0.2 检查工作区在宽视口底部直接列出五种显示模式。新打开的模型默认收起地面网格和坐标轴，可在视口底部单独开启。空间紧张时点击当前模式图标，或聚焦后按向下键展开菜单，用方向键选择、回车确认、Esc 关闭。

| 模式 | 行为 |
| --- | --- |
| 材质 | 保留原材质、贴图、透明度及既有灯光行为。 |
| 棋盘格材质 | 忽略原材质及透明度，使用带光照的程序化灰色棋盘格；有 UV 时按 UV 显示，无 UV 时根据表面方向投影到模型空间。采用导数过滤降低远处闪烁。 |
| 素模 | 使用统一的中性材质和光照观察表面起伏。 |
| 顶点色显示 | 忽略材质、贴图、光照和透明度，显示第一组源顶点 RGB；未提供该数据时使用中性灰，不将材质色冒充顶点色。 |
| 线框显示 | 沿用现有 GPU 线框实现，并使用可见的浅色、不透明线条。 |

FBX 从 `colour/0` 读取第一组角点色，glTF 从 `COLOR_0` 读取，ActorX 从 wedge 颜色读取。颜色以独立属性传入静态、GPU 骨骼和流式几何渲染；保留原默认着色数据。当前未解码源颜色的适配器显示中性灰。顶点 alpha 不用于此 RGB 检查模式。

切换显示模式仅更新渲染状态，不重建模型、不移动相机、不修改原始文件及导出内容。打开新文档恢复默认。UV 视图、源权重热图、地面网格和坐标轴保持各自检查语义；线框仍可用于三维权重视图。棋盘格复用当前三维材质所选 UV 与其变换，不替代独立 UV 检查视图。

### 骨骼、绑定与 LOD

「骨骼」和「绑定」在三维视口叠加浅蓝色的父子骨骼连线。动画模型按当前姿态更新，选中的骨骼仍以橙色突出显示。「骨骼」视图左下角显示当前骨骼名称。连线依据源文件中的直接父子骨骼关系，刚体节点动画可能有可播放片段但没有骨骼连线。

「LOD / 碰撞」在视口内显示当前碰撞模式和可见三角形数。切回「模型」恢复常规视图。浅色和深色主题各自使用 HTML 原型的视口底色。

### 验证入口

- `formaview_model_render_tests OUTPUT_JSON`：实际 D3D11 像素回读，包括黑色/全透明材质下的独立 RGB、UV/空间棋盘格、不透明覆盖、UV 隔离及原材质回归。
- `formaview_model_state_tests FIXTURES UFBX_FIXTURES BLENDER_FIXTURES`：源顶点 RGB 与材质乘色分离、原模型状态回归。
- `FORMAVIEW_MODEL_PREVIEW_SAMPLE` 指向只读问题 FBX，设置隔离 `FORMAVIEW_UI_TEST_ROOT`，运行 `formaview_desktop_tests modelPreviewDisplayModes`：五种模式、紧凑菜单、相机/几何不变、文档复位及截图。
- `formaview_model_gpu_tests SAMPLE OUTPUT_JSON 1 numeric`：既有骨骼 GPU 与 CPU 数值对照，覆盖新增顶点布局。

v0.2 第四阶段使用 HTML 原型中的同一份 `RobotExpressive.glb` 对照，原型列出 14 个片段，桌面端打开后选中 `Idle`，视口显示 3,237 个三角形。用 `FORMAVIEW_REVIEW_ANIMATED_MODEL` 指定该文件或 `Cheering.fbx`，运行 `formaview_desktop_tests modelAnimationInspectorControls` 可复查动画检查器、步进与拖动定格、LOD 切页及截图。有蒙皮绑定的样本还检查骨骼连线。Windows Release 下分别验证了 D3D11 GPU 蒙皮与强制 CPU 回退。

原始问题样本：`SK_Claymore_PhantomCicada.FBX`，27,733 点、31,715 三角形。样本及测试截图、日志仅保存在忽略的本地 `development/` 中。

三维视口的 N 和 T 可独立开启。蓝线为法线，红线优先显示源切线，缺失时用当前 UV 集计算。状态行标明切线来源。网格按三角形角点取样，最多选取 512 个三角形；动画随当前姿态更新。
