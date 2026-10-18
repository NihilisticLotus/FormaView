param(
    [ValidateSet('Configure', 'Build', 'Test', 'Run', 'Smoke', 'Bench', 'Lint')]
    [string]$Action = 'Build',
    [string]$QtRoot = $env:FORMAVIEW_QT_ROOT
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio C++ tools were not found.' }
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not $QtRoot -or -not (Test-Path -LiteralPath (Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw 'Set FORMAVIEW_QT_ROOT or pass -QtRoot for Qt 6.12.0 msvc2022_64.'
}
$previousPath = $env:PATH
$previousQt = $env:FORMAVIEW_QT_ROOT
$previousPluginPath = $env:QT_PLUGIN_PATH
$previousQmlPath = $env:QML_IMPORT_PATH
Push-Location -LiteralPath $repo
try {
    $env:FORMAVIEW_QT_ROOT = (Resolve-Path -LiteralPath $QtRoot).Path
    $env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
    $env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'
    $env:QML_IMPORT_PATH = Join-Path $QtRoot 'qml'
    switch ($Action) {
        Configure { & $cmake --preset windows-msvc -DFORMAVIEW_BUILD_HOST=ON }
        Build { & $cmake --build --preset release --parallel }
        Test { & $ctest --preset release }
        Run { & '.\out\msvc\Release\formaview.exe' }
        Smoke { & '.\out\msvc\Release\formaview.exe' --smoke }
        Bench { & '.\out\msvc\tests\Release\formaview_core_bench.exe' }
        Lint { & $cmake --build --preset release --target formaview_qmllint }
    }
    if ($LASTEXITCODE -ne 0) { throw "$Action failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
    $env:PATH = $previousPath
    $env:FORMAVIEW_QT_ROOT = $previousQt
    $env:QT_PLUGIN_PATH = $previousPluginPath
    $env:QML_IMPORT_PATH = $previousQmlPath
}
