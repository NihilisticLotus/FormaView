<#
.SYNOPSIS
  Verifies a run directory made by package_runtime.ps1 in a minimal process
  environment (PATH contains only System32/Windows, no Qt or SDK locations).
#>
param(
    [Parameter(Mandatory)][string]$RunDir,
    [Parameter(Mandatory)][string]$EvidenceDir,
    [string]$SourceRoot,
    [switch]$ManifestOnly
)
$ErrorActionPreference = 'Stop'
$RunDir = [IO.Path]::GetFullPath($RunDir)
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$report = [ordered]@{ run_dir = $RunDir; checks = @() }
function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $script:report.checks += [ordered]@{ name = $name; ok = $ok; detail = $detail }
    Write-Host ("{0} {1} {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail)
}

# 1. Manifest hashes.
$manifestName = if (Test-Path -LiteralPath (Join-Path $RunDir 'payload-manifest.json')) { 'payload-manifest.json' } else { 'sha256.json' }
$document = Get-Content -Raw -LiteralPath (Join-Path $RunDir $manifestName) | ConvertFrom-Json
$manifest = if ($manifestName -eq 'payload-manifest.json') { @($document.files) } else { @($document) }
$invalidPaths = @($manifest | Where-Object {
    $path = $_.path
    $path -isnot [string] -or -not $path -or $path -match '[\\:\x00-\x1f]' -or
        @($path -split '/' | Where-Object { -not $_ -or $_ -eq '.' -or $_ -eq '..' }).Count -gt 0
})
if ($invalidPaths.Count) {
    Add-Check 'manifest-paths' $false "invalid=$($invalidPaths.Count)"
    $report | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 (Join-Path $EvidenceDir 'verify-runtime.json')
    exit 1
}
$bad = @($manifest | Where-Object {
    $p = Join-Path $RunDir $_.path
    -not (Test-Path -LiteralPath $p -PathType Leaf) -or (Get-Item -LiteralPath $p).Length -ne $_.bytes -or (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() -ne $_.sha256 })
$listed = @($manifest.path)
$unlisted = @(Get-ChildItem -LiteralPath $RunDir -Recurse -File | ForEach-Object { $_.FullName.Substring($RunDir.TrimEnd('\').Length + 1).Replace('\', '/') } |
    Where-Object { $_ -ne $manifestName -and $_ -notin $listed })
$duplicates = @($listed | Group-Object | Where-Object Count -gt 1)
Add-Check 'manifest' (($manifest.Count -gt 0) -and ($bad.Count -eq 0) -and ($unlisted.Count -eq 0) -and ($duplicates.Count -eq 0)) "files=$($manifest.Count) mismatched=$($bad.Count) unlisted=$($unlisted.Count) duplicates=$($duplicates.Count)"

# A self-consistent manifest alone cannot detect a runtime component omitted
# from both the directory and the manifest. Keep these obligations independent.
$sourceFiles = [ordered]@{
    'mcp/image_stdio.py' = 'tools/mcp/image_stdio.py'
    'mcp/requirements.lock' = 'tools/mcp/requirements.lock'
    'mcp/requirements.txt' = 'tools/mcp/requirements.txt'
    'README-run.txt' = 'tools/runtime/README-run.txt'
}
$required = @('formaview.exe', 'formaview_image_service.exe', 'formaview_image_cli.exe') + @($sourceFiles.Keys)
$absent = @($required | Where-Object { $_ -notin $listed -or -not (Test-Path -LiteralPath (Join-Path $RunDir $_) -PathType Leaf) })
Add-Check 'required-runtime-files' ($absent.Count -eq 0) ("missing=" + ($absent -join '; '))
if ($SourceRoot) {
    $different = @($sourceFiles.Keys | Where-Object {
        $packed = Join-Path $RunDir $_
        $source = Join-Path $SourceRoot $sourceFiles[$_]
        -not (Test-Path -LiteralPath $packed -PathType Leaf) -or -not (Test-Path -LiteralPath $source -PathType Leaf) -or
            (Get-FileHash -LiteralPath $packed -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    })
    Add-Check 'packaged-source-files' ($different.Count -eq 0) ("different=" + ($different -join '; '))
}
if ($ManifestOnly -or ($report.checks | Where-Object { -not $_.ok })) {
    $report | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 (Join-Path $EvidenceDir 'verify-runtime.json')
    if ($report.checks | Where-Object { -not $_.ok }) { exit 1 }
    exit 0
}

# 2. Every import resolves inside the directory or System32.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dumpbin = Get-ChildItem -Path (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe | Where-Object FullName -Match 'Hostx64\\x64' | Select-Object -First 1 -ExpandProperty FullName
$system = Join-Path $env:WINDIR 'System32'
$missing = [System.Collections.Generic.HashSet[string]]::new()
$inspectionFailures = [System.Collections.Generic.List[string]]::new()
Get-ChildItem -LiteralPath $RunDir -Recurse -Include *.exe, *.dll | ForEach-Object {
    $owner = $_.Name
    $imports = & $dumpbin /dependents $_.FullName
    if ($LASTEXITCODE -ne 0) {
        $inspectionFailures.Add("$($_.FullName) (exit $LASTEXITCODE)")
        return
    }
    $imports | ForEach-Object {
        if ($_ -match '^\s+(\S+\.dll)\s*$') {
            $d = $Matches[1]
            $appLocal = $d -match '^(msvcp|vcruntime|concrt|vccorlib)\d+'  # must not depend on an installed redistributable
            if ($d -notlike 'api-ms-win-*' -and ($appLocal -or -not (Test-Path (Join-Path $system $d))) -and -not (Test-Path (Join-Path $RunDir $d))) { [void]$missing.Add("$d <- $owner") }
        }
    }
}
Add-Check 'imports-inspected' ($inspectionFailures.Count -eq 0) ("failed=" + ($inspectionFailures -join '; '))
Add-Check 'imports-resolved' (($inspectionFailures.Count -eq 0) -and ($missing.Count -eq 0)) ("unresolved=" + ($missing -join '; '))
if ($report.checks | Where-Object { -not $_.ok }) {
    $report | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 (Join-Path $EvidenceDir 'verify-runtime.json')
    exit 1
}

# 3. Start with a minimal environment.
$minimalPath = "$env:WINDIR\System32;$env:WINDIR"
function Run-Clean([string[]]$arguments, [string]$name) {
    $start = [System.Diagnostics.ProcessStartInfo]::new((Join-Path $RunDir 'formaview.exe'))
    $start.Arguments = ($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $start.WorkingDirectory = $EvidenceDir
    foreach ($key in @($start.Environment.Keys)) { if ($key -notin 'SystemRoot', 'windir', 'TEMP', 'TMP', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'COMSPEC', 'PATHEXT') { [void]$start.Environment.Remove($key) } }
    $start.Environment['PATH'] = $minimalPath
    $process = [System.Diagnostics.Process]::Start($start)
    $out = $process.StandardOutput.ReadToEndAsync(); $err = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(60000)) { $process.Kill(); throw "$name timed out" }
    Set-Content -LiteralPath (Join-Path $EvidenceDir "$name.stdout.txt") -Value $out.Result -Encoding utf8
    Set-Content -LiteralPath (Join-Path $EvidenceDir "$name.stderr.txt") -Value $err.Result -Encoding utf8
    [pscustomobject]@{ Exit = $process.ExitCode; Out = $out.Result; Err = $err.Result }
}
$smoke = Run-Clean @('--smoke') 'smoke'
Add-Check 'smoke' (($smoke.Exit -eq 0) -and ($smoke.Out -match 'HOST_READY rendered_frame=1')) "exit=$($smoke.Exit) $($smoke.Out.Trim())"
$caps = Run-Clean @('--capabilities') 'capabilities'
$capsJson = $caps.Out | ConvertFrom-Json
Add-Check 'capabilities' (($caps.Exit -eq 0) -and ($capsJson.read_formats.Count -eq 11) -and ($capsJson.write_targets.Count -eq 9) -and ($capsJson.model_export.output_formats -contains 'fbx')) "read=$($capsJson.read_formats.Count) write=$($capsJson.write_targets.Count)"
$report | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 (Join-Path $EvidenceDir 'verify-runtime.json')
if ($report.checks | Where-Object { -not $_.ok }) { exit 1 }
