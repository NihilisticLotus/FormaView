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
