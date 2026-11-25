# Build-time tools only. The delivered stdio command runs without a system Python.
param([Parameter(Mandatory)][string]$OutDir, [Parameter(Mandatory)][string]$WorkDir)
$ErrorActionPreference = 'Stop'
$OutDir = [IO.Path]::GetFullPath($OutDir)
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$venv = Join-Path $WorkDir 'venv'
$python = Join-Path $venv 'Scripts/python.exe'
if (!(Test-Path -LiteralPath $python)) {
    & uv venv --python 3.12 $venv
    if ($LASTEXITCODE) { throw 'Cannot create the MCP build environment' }
}
& uv pip install --python $python --require-hashes -r "$PSScriptRoot/requirements.lock"
if ($LASTEXITCODE) { throw 'Cannot install the locked MCP dependencies' }
& uv pip install --python $python pyinstaller==6.19.0
if ($LASTEXITCODE) { throw 'Cannot install the MCP packager' }
& $python -m PyInstaller --noconfirm --onefile --console --noupx --name formaview-mcp --copy-metadata mcp --collect-data mcp --hidden-import win32timezone --distpath $OutDir --specpath $WorkDir --workpath "$WorkDir/build" "$PSScriptRoot/image_stdio.py"
if ($LASTEXITCODE) { throw 'Cannot build the standalone MCP adapter' }
# Preserve the license notices for the bundled Python runtime and distributions.
$licenses = Join-Path $OutDir 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
$base = & $python -c 'import sys; print(sys.base_prefix)'
Copy-Item -LiteralPath "$base/LICENSE.txt" -Destination "$licenses/python.txt"
$site = Join-Path $venv 'Lib/site-packages'
Get-ChildItem -LiteralPath $site -Directory -Filter '*.dist-info' | ForEach-Object {
    $package = $_
    Get-ChildItem -LiteralPath $package.FullName -File -Recurse | Where-Object { $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)' } | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($package.FullName, $_.FullName)
        $target = Join-Path (Join-Path $licenses $package.Name) $relative
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target
    }
}
