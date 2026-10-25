<#
.SYNOPSIS
  Assembles a self-contained functional run directory (not an installer).
.DESCRIPTION
  Copies the built host, image service and CLI, runs windeployqt for the Qt
  runtime/plugins/QML, resolves third-party DLLs by scanning import tables
  (dumpbin) against -ExtraDllDir, and adds the MCP adapter, lock file and
  third-party notices. The result starts without any PATH changes; verify with
  tools/verify_runtime.ps1. A real installer is a separate, later deliverable.
#>
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$OutDir,
    [string]$QtRoot = $env:FORMAVIEW_QT_ROOT,
    [string]$ExtraDllDir,
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $QtRoot) { throw 'Set FORMAVIEW_QT_ROOT or pass -QtRoot.' }
$bin = Join-Path $BuildDir $Configuration
$required = 'formaview.exe', 'formaview_image_service.exe', 'formaview_image_cli.exe'
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $name))) { throw "Missing built file: $name" }
}
if ((Test-Path -LiteralPath $OutDir) -and (Get-ChildItem -LiteralPath $OutDir -Force | Select-Object -First 1)) {
    throw "Output directory is not empty: $OutDir"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
foreach ($name in $required) { Copy-Item -LiteralPath (Join-Path $bin $name) -Destination $OutDir }
Get-ChildItem -LiteralPath $bin -Filter 'FormaView-*-LICENSE.txt' | Copy-Item -Destination $OutDir

$deploy = Join-Path $QtRoot 'bin\windeployqt.exe'
& $deploy --release --no-translations --no-system-d3d-compiler --compiler-runtime `
    --qmldir (Join-Path $repo 'src\host') --dir $OutDir (Join-Path $OutDir 'formaview.exe') | Out-File -Encoding utf8 (Join-Path $OutDir 'qt-deployment.log')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE)" }
# The image service reuses the host's Qt image-format plugins and runtime.
# Release builds never expose QML debugging services: drop the tooling plugins.
Remove-Item -LiteralPath (Join-Path $OutDir 'qmltooling') -Recurse -Force -ErrorAction SilentlyContinue

# Static QML plugin type information used by the compiled FormaView modules.
foreach ($module in 'Images', 'Models') {
    $source = Join-Path $BuildDir "src\host\FormaView\$module"
    if (Test-Path -LiteralPath $source) {
        $target = Join-Path $OutDir "qml\FormaView\$module"
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Name -in 'qmldir' -or $_.Extension -eq '.qmltypes' } |
            Copy-Item -Destination $target
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dumpbin = Get-ChildItem -Path (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe |
    Where-Object FullName -Match 'Hostx64\\x64' | Select-Object -First 1 -ExpandProperty FullName
function Get-Imports([string]$file) {
    & $dumpbin /dependents $file | ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1] } }
}
$system = Join-Path $env:WINDIR 'System32'
# App-local Visual C++ runtime so a clean machine needs no redistributable install.
$crt = Get-ChildItem -Path (Join-Path $vs 'VC\Redist\MSVC') -Recurse -Directory -Filter 'Microsoft.VC143.CRT' |
    Where-Object FullName -Match 'x64' | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'Microsoft.VC143.CRT x64 redistributable files were not found.' }
Copy-Item -Path (Join-Path $crt.FullName '*.dll') -Destination $OutDir
$queue = [System.Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $OutDir -Recurse -Include *.exe, *.dll | ForEach-Object { $queue.Enqueue($_.FullName) }
$seen = @{}
while ($queue.Count) {
    $file = $queue.Dequeue()
    if ($seen[$file]) { continue }
    $seen[$file] = $true
    foreach ($dll in Get-Imports $file) {
        if ($dll -like 'api-ms-win-*' -or (Test-Path -LiteralPath (Join-Path $system $dll))) { continue }
        if (Test-Path -LiteralPath (Join-Path $OutDir $dll)) { continue }
        $found = if ($ExtraDllDir) { Join-Path $ExtraDllDir $dll } else { $null }
        if ($found -and (Test-Path -LiteralPath $found)) {
            Copy-Item -LiteralPath $found -Destination $OutDir
            $queue.Enqueue((Join-Path $OutDir $dll))
        } else { Write-Warning "Unresolved import $dll required by $(Split-Path -Leaf $file)" }
    }
}

New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'mcp'), (Join-Path $OutDir 'licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'tools\mcp\image_stdio.py'), (Join-Path $repo 'tools\mcp\requirements.lock'), (Join-Path $repo 'tools\mcp\requirements.txt') -Destination (Join-Path $OutDir 'mcp')
Copy-Item -Path (Join-Path $repo 'third_party\*') -Destination (Join-Path $OutDir 'licenses') -Include *.txt
Copy-Item -LiteralPath (Join-Path $repo 'tools\runtime\README-run.txt') -Destination (Join-Path $OutDir 'README-run.txt')

$files = Get-ChildItem -LiteralPath $OutDir -Recurse -File | Where-Object Name -NotIn 'sha256.json' | Sort-Object FullName
$manifest = foreach ($file in $files) {
    [ordered]@{
        path = $file.FullName.Substring($OutDir.TrimEnd('\', '/').Length + 1).Replace('\', '/')
        bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest | ConvertTo-Json -Depth 3 | Out-File -Encoding utf8 (Join-Path $OutDir 'sha256.json')
"Runtime directory ready: $OutDir ($($files.Count) files)"
