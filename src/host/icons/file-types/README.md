# 文件类型图标

2026-11-23 用户确认的 FormaView 三类文件图标：视频为大播放三角，图片为山景与太阳，模型为独立立方体。延续正式品牌的蓝橙立体材质；不替换应用品牌或动作按钮。

主资源在独立图标设计会话中生成并获用户认可，来源归档名为 FormaView-FileType-Icons-20261123。此目录完整保存主图、派生图和可重建脚本，无需依赖该外部归档。

- `*-master.png`：透明 RGBA 主图，1254 × 1254。
- `*-<size>.png`：16/20/24/32/40/48/64/128/256/512px。
- `*.ico`：九层 16–256px，32-bit Alpha。
- `export-icons.ps1`：使用 Windows System.Drawing 高质量双三次缩放，生成 PNG 与 PNG 编码 ICO。
- `manifest.json`：三类 ICO 的 SHA-256、尺寸和字节数。

在 Windows 上运行 `& ./src/host/icons/file-types/export-icons.ps1` 即可原地重建。修改主图需重新生成派生资源与 manifest，并运行 `tests/file_type_icon_tests.py`。2026-11-23 已在仓库外复制主图后重建，三份 ICO 与提交资源逐字节一致。

分类、Qt 高 DPI 取样、安装器哈希文件名和升级保护说明见 [icon-alignment.md](../../../../docs/icon-alignment.md)；本次原生验证及系统验收边界见 [release-validation.md](../../../../docs/release-validation.md)。
