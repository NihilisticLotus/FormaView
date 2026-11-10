param([Parameter(Mandatory)][string]$StagingRoot)
$ErrorActionPreference = 'Stop'
$dev = [IO.Path]::GetFullPath($StagingRoot)
$evidence = Join-Path $dev 'verification/M13-upgrade-policy-20261111'
$build = Get-Content -LiteralPath (Join-Path $evidence 'installer-build-record.json') -Raw | ConvertFrom-Json
$qa = Get-Content -LiteralPath (Join-Path $evidence 'verification.json') -Raw | ConvertFrom-Json
$versions = Get-Content -LiteralPath (Join-Path $evidence 'version-verification.json') -Raw | ConvertFrom-Json
$runtime = Get-Content -LiteralPath $build.runtimeDelivery -Raw | ConvertFrom-Json
$release = (Resolve-Path -LiteralPath $build.installer).Path
$old = Join-Path $dev 'deploy/FormaView-0.1.0-Windows-x64-uninstall-options-20261111/output/FormaView-0.1.0-Windows-x64-Setup.exe'
$fixtureNames = @('wizard-fixture.exe', 'wizard-legacy.exe', 'version-0.1.0.exe', 'version-0.1.5.exe', 'version-0.3.0.exe', 'version-abort.exe')

function Assert-DeliveryHashes {
    if (-not $qa.passed -or $qa.releaseSha256 -ne $build.sha256 -or
        -not $versions.passed -or $versions.releaseSha256 -ne $build.sha256 -or
        (Get-FileHash -LiteralPath $release -Algorithm SHA256).Hash -ne $build.sha256 -or
        (Get-FileHash -LiteralPath $runtime.archive -Algorithm SHA256).Hash -ne $build.runtimeZipSha256 -or
        (Get-FileHash -LiteralPath (Join-Path $runtime.runDirectory 'sha256.json') -Algorithm SHA256).Hash -ne $runtime.manifestSha256) {
        throw 'Delivery files differ from the successful build or acceptance records.'
    }
}

Assert-DeliveryHashes
$signature = (Get-AuthenticodeSignature -LiteralPath $release).Status.ToString()
$processes = Get-CimInstance Win32_Process
$candidates = [Collections.Generic.List[string]]::new()
if (Test-Path -LiteralPath $old -PathType Leaf) {
    if ((Get-FileHash -LiteralPath $old -Algorithm SHA256).Hash -ne 'fa09f7ffb89ed3f357054afcde2402e3461b03862e7aa4db5226bc0ee4bdc2b8') {
        throw 'Previous installer does not match its known SHA-256.'
    }
    $candidates.Add($old)
}
foreach ($fixture in Get-ChildItem -LiteralPath $evidence -Directory) {
    if ($fixture.Name -notmatch '^fixture-[0-9a-f]{12}$' -or ($fixture.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        continue
    }
    foreach ($name in $fixtureNames) {
        $target = Join-Path $fixture.FullName $name
        if (Test-Path -LiteralPath $target -PathType Leaf) { $candidates.Add($target) }
    }
}

$removed = @()
$deferred = @()
foreach ($candidate in $candidates) {
    $target = (Resolve-Path -LiteralPath $candidate).Path
    $file = Get-Item -LiteralPath $target
    $isOld = $target -eq [IO.Path]::GetFullPath($old)
    $isFixture = $file.Directory.Parent.FullName -eq $evidence -and
        $file.Directory.Name -match '^fixture-[0-9a-f]{12}$' -and
        $file.Name -in $fixtureNames
    if ($target -eq $release -or (-not $isOld -and -not $isFixture) -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Cleanup target failed scope validation: $target"
    }
    $item = [ordered]@{ path = $target; bytes = $file.Length; sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() }
    if ($processes | Where-Object { $_.ExecutablePath -eq $target -or ($_.Name -like '*.tmp' -and $_.CommandLine -like "*$target*") }) {
        $deferred += $item
        continue
    }
    Remove-Item -LiteralPath $target
    if (Test-Path -LiteralPath $target) { throw "Installer remains after cleanup: $target" }
    $removed += $item
}

Assert-DeliveryHashes
$report = Join-Path $evidence '验收报告.md'
$runtimeReport = Join-Path (Split-Path -Parent $build.runtimeDelivery) '完成报告.md'
$pointer = Join-Path $dev 'deploy/当前人工验证版.txt'
@"
当前开发验证版：任务历史隔离（f19c5baf7450，2026-11-10）
运行目录：$($runtime.runDirectory)
ZIP：$($runtime.archive)
ZIP SHA-256：$($runtime.archiveSha256)
安装包：$release
安装包 SHA-256：$($build.sha256)
跨版本覆盖及卸载验收：$report
开发验证版验收：$runtimeReport
首次安装推荐为本机所有用户安装；跨版本覆盖沿用原范围和目录并保留用户数据。卸载可独立选择保留设置和历史，默认两项都保留。安装包未签名。
"@ | Set-Content -LiteralPath $pointer -Encoding utf8
[ordered]@{
    removed = $removed
    deferred = $deferred
    removedBytes = ($removed | ForEach-Object { $_.bytes } | Measure-Object -Sum).Sum
    retainedInstaller = $release
    retainedInstallerSha256 = $build.sha256
    retainedRuntimeZip = $runtime.archive
    retainedRuntimeZipSha256 = $runtime.archiveSha256
    authenticodeStatus = $signature
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidence 'cleanup.json') -Encoding utf8
Write-Output "Verified delivery; signature: $signature; removed: $($removed.Count); deferred: $($deferred.Count)"
