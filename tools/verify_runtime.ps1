<#
.SYNOPSIS
  Verifies a run directory made by package_runtime.ps1 in a minimal process
  environment (PATH contains only System32/Windows, no Qt or SDK locations).
#>
param(
    [Parameter(Mandatory)][string]$RunDir,
    [Parameter(Mandatory)][string]$EvidenceDir
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$report = [ordered]@{ run_dir = $RunDir; checks = @() }
function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $script:report.checks += [ordered]@{ name = $name; ok = $ok; detail = $detail }
    Write-Host ("{0} {1} {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail)
}

# 1. Manifest hashes.
$manifest = Get-Content -Raw -LiteralPath (Join-Path $RunDir 'sha256.json') | ConvertFrom-Json
$bad = @($manifest | Where-Object {
    $p = Join-Path $RunDir $_.path
    -not (Test-Path -LiteralPath $p) -or (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() -ne $_.sha256 })
$listed = @($manifest.path)
$unlisted = @(Get-ChildItem -LiteralPath $RunDir -Recurse -File | ForEach-Object { $_.FullName.Substring($RunDir.TrimEnd('\').Length + 1).Replace('\', '/') } |
    Where-Object { $_ -ne 'sha256.json' -and $_ -notin $listed })
Add-Check 'manifest' (($bad.Count -eq 0) -and ($unlisted.Count -eq 0)) "files=$($manifest.Count) mismatched=$($bad.Count) unlisted=$($unlisted.Count)"

# 2. Every import resolves inside the directory or System32.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dumpbin = Get-ChildItem -Path (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe | Where-Object FullName -Match 'Hostx64\\x64' | Select-Object -First 1 -ExpandProperty FullName
$system = Join-Path $env:WINDIR 'System32'
$missing = [System.Collections.Generic.HashSet[string]]::new()
Get-ChildItem -LiteralPath $RunDir -Recurse -Include *.exe, *.dll | ForEach-Object {
    $owner = $_.Name
    & $dumpbin /dependents $_.FullName | ForEach-Object {
        if ($_ -match '^\s+(\S+\.dll)\s*$') {
            $d = $Matches[1]
            $appLocal = $d -match '^(msvcp|vcruntime|concrt|vccorlib)\d+'  # must not depend on an installed redistributable
            if ($d -notlike 'api-ms-win-*' -and ($appLocal -or -not (Test-Path (Join-Path $system $d))) -and -not (Test-Path (Join-Path $RunDir $d))) { [void]$missing.Add("$d <- $owner") }
        }
    }
}
Add-Check 'imports-resolved' ($missing.Count -eq 0) ("unresolved=" + ($missing -join '; '))

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
