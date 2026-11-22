# Run as the account that installed FormaView. The official 0.1.0 wizard
# rolls back a selected registration when its legacy icon differs from v0.2.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Installer, [switch]$Silent)
$ErrorActionPreference = 'Stop'
$setup = Get-Item -LiteralPath $Installer
$official = '5ecf968cfbda35fe56ad39d71f71eece573e7f342dbcd88eef218f6d3bfb4a1c'
if ((Get-FileHash -LiteralPath $setup.FullName).Hash -ne $official) {
    throw 'Select the official FormaView 0.1.0 Windows x64 installer.'
}
$key = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{618397E7-C70C-4D14-B601-D6FB2F562BA1}_is1'
$view = [Microsoft.Win32.RegistryView]::Registry64
$user = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', $view)
$machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $view)
$current = $user.OpenSubKey($key)
if ($machine.OpenSubKey($key) -or !$current -or $current.GetValue('DisplayVersion') -ne '0.2.0') {
    throw 'This rollback requires a current-user FormaView 0.2.0 installation.'
}
$caps = $user.OpenSubKey('SOFTWARE\FormaView\LocalFileCapabilities\FileAssociations')
$expectedCommand = '"{0}" --open "%1"' -f (Join-Path $current.GetValue('InstallLocation') 'formaview.exe')
$selected = if ($caps) {
    $caps.GetValueNames() | Where-Object {
        $_ -match '^\.[a-z0-9]+$' -and $caps.GetValue($_) -eq "FormaView.Local$_"
    } | Sort-Object
}
$options = @('/CURRENTUSER', '/NORESTART', ('/ASSOC=' + ($selected -join ',')), '/SUPPRESSMSGBOXES')
$launch = @{}
if ($Silent) {
    $options += '/VERYSILENT'
    $launch.WindowStyle = 'Hidden'
} else {
    $options += '/SILENT'
}
function Test-Restored {
    $installed = $user.OpenSubKey($key)
    if (!$installed -or $installed.GetValue('DisplayVersion') -ne '0.1.0') { return $false }
    $after = $user.OpenSubKey('SOFTWARE\FormaView\LocalFileCapabilities\FileAssociations')
    foreach ($ext in $selected) {
        $progId = "FormaView.Local$ext"
        $cmd = $user.OpenSubKey("Software\Classes\$progId\shell\open\command")
        $openWith = $user.OpenSubKey("Software\Classes\$ext\OpenWithProgids")
        if (!$after -or $after.GetValue($ext) -ne $progId -or !$cmd -or $cmd.GetValue('') -ne $expectedCommand -or
            !$openWith -or $progId -notin $openWith.GetValueNames()) { return $false }
    }
    return $true
}
for ($attempt = 1; $attempt -le 2; $attempt++) {
    $result = Start-Process -FilePath $setup.FullName -ArgumentList $options -Wait -PassThru @launch
    if ($result.ExitCode) { exit $result.ExitCode }
    if (Test-Restored) { exit 0 }
    if ($attempt -eq 1) {
        Write-Output 'The legacy installer rolled back an association. Repairing with the same verified installer.'
    }
}
throw 'The legacy installer could not restore all selected file associations.'
