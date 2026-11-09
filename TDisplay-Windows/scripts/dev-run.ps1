# Development loop: rebuilds everything, pushes the tablet app and restarts the laptop program.
param(
    [switch]$SkipApp
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($SkipApp) { & "$PSScriptRoot\build.ps1" -HostOnly } else { & "$PSScriptRoot\build.ps1" }
$adb = "$root\dist\adb\adb.exe"
if (-not $SkipApp) {
    & $adb -P 15037 install -r "$root\dist\TDisplay.apk"
    if ($LASTEXITCODE -ne 0) { throw 'install failed' }
}
Start-Process "$root\dist\tdisplay-host.exe"
