# Run in a disposable Windows Sandbox with a freshly logged-on standard token.
param(
    [Parameter(Mandatory)][string]$OldSetup,
    [Parameter(Mandatory)][string]$NewSetup,
    [Parameter(Mandatory)][string]$Evidence
)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -or
    $principal.Identity.User.Value -notlike '*-504' -or (whoami /groups) -match 'S-1-5-32-544') {
    throw 'Use the ordinary, non-Administrators Sandbox account'
}
$install = Join-Path $env:LOCALAPPDATA 'Programs\FormaView'
$fixture = Join-Path $env:USERPROFILE 'FV-ShellUpgrade'
if ((Test-Path $install) -or (Test-Path $fixture) -or (Test-Path 'HKCU:\Software\Classes\.pskx')) {
    throw 'This check requires a fresh Sandbox profile'
}
New-Item -ItemType Directory -Path $fixture | Out-Null
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null

function Read-Choices {
    foreach ($ext in '.png','.jpg','.mp4','.flv','.fbx','.pskx') {
        $value = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext\UserChoice" -ErrorAction SilentlyContinue
        [pscustomobject]@{extension=$ext; progId=$value.ProgId; hash=$value.Hash}
    }
}
function Test-Dispatch([string]$stage) {
    $receipt = Join-Path $fixture 'opened.txt'
    $sample = Join-Path $fixture "$stage sample.pskx"
    Set-Content -LiteralPath $sample -Value $stage
    Start-Process -FilePath $sample -Verb open
    for ($i=0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 100
        if ((Test-Path $receipt) -and [IO.File]::ReadAllText($receipt) -ceq $sample) { return }
    }
    throw "$stage Shell open did not reach the original viewer"
}
function Install-Package([string]$setup,[string]$formats,[string]$stage) {
    $log = Join-Path $Evidence "$stage.log"
    $proc = Start-Process -FilePath $setup -ArgumentList "/CURRENTUSER /VERYSILENT /NORESTART /SUPPRESSMSGBOXES /ASSOC=$formats /LOG=""$log""" -Wait -PassThru -WindowStyle Hidden
    if ($proc.ExitCode) { throw "$stage installer exited with $($proc.ExitCode)" }
}
$baseline = @(Read-Choices)
if (!($baseline | Where-Object { $_.progId -and $_.hash })) { throw 'Expected an existing protected default for the control group' }
Install-Package $OldSetup '.png,.dds,.fbx' 'v012'
$oldIcon = (Get-Item 'HKCU:\Software\Classes\FormaView.Local.dds\DefaultIcon').GetValue('')
$handler = Join-Path $fixture 'PreviousViewer.exe'
Add-Type -TypeDefinition @'
using System;
using System.IO;
public class PreviousViewer {
    public static void Main(string[] args) {
        File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"opened.txt"),args[0]);
    }
}
'@ -OutputAssembly $handler -OutputType WindowsApplication
$foreign = 'FVTest.PreviousViewer'
$classes = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Classes')
$values = @{
    '.pskx' = @('', $foreign)
    '.pskx\OpenWithProgids' = @($foreign, '')
    "$foreign\shell\open\command" = @('', ('"' + $handler + '" "%1"'))
}
foreach ($entry in $values.GetEnumerator()) {
    $key = $classes.CreateSubKey($entry.Key)
    try { $key.SetValue($entry.Value[0], $entry.Value[1]) } finally { $key.Close() }
}
$classes.Close()
Test-Dispatch 'before'
Install-Package $NewSetup '.png,.dds,.flv,.fbx,.pskx' 'v020'
Test-Dispatch 'after'
$after = @(Read-Choices)
if ((ConvertTo-Json $baseline) -cne (ConvertTo-Json $after)) { throw 'Protected defaults changed' }
$icons = foreach ($entry in @{dds='image';flv='video';fbx='model'}.GetEnumerator()) {
    $path = (Get-Item "HKCU:\Software\Classes\FormaView.Local.$($entry.Key)\DefaultIcon").GetValue('')
    if ($path -eq $oldIcon -or $path -notmatch '^"(.+\\formaview-file-([a-z]+)-icon-([0-9a-f]{64})\.ico)",0$') { throw "Unexpected $($entry.Key) icon: $path" }
    if ($Matches[2] -cne $entry.Value -or (Get-FileHash -LiteralPath $Matches[1]).Hash -ne $Matches[3]) { throw "Wrong $($entry.Key) category or file contents" }
    [pscustomobject]@{extension=$entry.Key; defaultIcon=$path}
}
[pscustomobject]@{os=[Environment]::OSVersion.Version.ToString(); oldSetup=(Get-FileHash $OldSetup).Hash;
    newSetup=(Get-FileHash $NewSetup).Hash; protectedBefore=$baseline; protectedAfter=$after;
    originalViewerReceivedBothFiles=$true; icons=$icons} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Evidence 'verified.json') -Encoding UTF8
