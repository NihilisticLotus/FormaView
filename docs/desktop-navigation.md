# 转到桌面

文件区标题右侧的显示器图标可直接转到当前用户的Windows桌面；悬停提示“转到桌面”，也可Tab聚焦后按Space或Enter。按钮在软件内更新目录、地址和文件列表，不弹出文件夹选择窗口。切换前保存当前文件视图；可用前进、后退恢复目录和选择。

每次点击通过`QStandardPaths::DesktopLocation`解析系统位置。在Windows上Qt使用`FOLDERID_Desktop`与`SHGetKnownFolderPath`，因此不依赖固定用户名、英文Desktop文件夹或默认用户目录，支持系统配置的OneDrive/其他桌面重定向。没有修改Windows桌面位置或注册表。

位置为空时取消未完成的目录扫描/过滤并明确报告桌面位置不可用；目录不存在、不可读等错误由原有异步目录扫描报告，保留当前列表和历史，不创建替代目录。连续点击复用导航修订号，只接受最新结果，不重复堆叠相同历史。错误界面保留可复制的原始诊断。

回归覆盖真实系统位置的只读扫描和QML Space触发，以及通过C++位置解析器注入的中文重定向目录、连续点击、前进/后退的选择与滚动恢复、缺失路径和空位置。注入只用于隔离验证，不暴露给QML；未改变本机Known Folder配置。证据位于仓库外`asset-preview-planning/development/verification/M12.5-ui-20261112/`。原生DPI和屏幕阅读器仍需人工验收。

Release主程序及桌面测试构建成功；`desktopQuickNavigation`、原目录导航隔离、文件视图和图标回归合计6通过、0失败/跳过（含初始化/清理），见`desktop-cancel-1/tests.txt`。包含扫描10024项时桌面位置突然为空：旧任务收到取消并结束，旧结果不覆盖目录或错误。离屏截图确认按钮与收起入口无重叠；软件渲染出现的No QRhi警告不构成原生GPU验收。

参考：[Qt QStandardPaths](https://doc.qt.io/qt-6/qstandardpaths.html)、[Qt Windows实现](https://github.com/qt/qtbase/blob/dev/src/corelib/io/qstandardpaths_win.cpp)。
