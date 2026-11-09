# Test helper: puts this PC and the connected tablet back to how a stranger's would look, so that
# dist\TDisplaySetup.exe gets a true first-time test. Removes TDisplay, its driver and settings, the app on the
# tablet, and Windows' trust in the driver's publisher. It doesn't touch anything else.
$root = Split-Path -Parent $PSScriptRoot
$installed = "$env:ProgramFiles\TDisplay\uninstall.exe"
$adb = "$root\dist\adb\adb.exe"

# The tablet goes first, while there's still an adb server that knows it.
Write-Host 'Removing the app from the tablet, if one is connected'
# Android won't uninstall a pinned app, and edge lock pins it. Unpinning locks a tablet that has a PIN or pattern.
& $adb -P 15037 shell am task lock stop 2>&1 | Out-Null
Start-Sleep -Seconds 1
& $adb -P 15037 uninstall com.tdisplay.windows 2>&1 | Out-String | Write-Host

if (Test-Path $installed) {
    Write-Host 'Running the uninstaller. Click Yes, then Close.'
    Start-Process $installed -ArgumentList '--uninstall' -Wait
    Start-Sleep -Seconds 4
} else {
    Write-Host 'Removing the driver that was installed for development'
    Get-Process tdisplay-host -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    & "$PSScriptRoot\install-driver.ps1" -Uninstall
}
& $adb -P 15037 kill-server 2>&1 | Out-Null
Remove-Item -Recurse -Force 'C:\VirtualDisplayDriver' -ErrorAction SilentlyContinue

Write-Host "Making Windows forget it trusted the driver's publisher"
Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object { $_.Subject -like '*SignPath Foundation*' } | Remove-Item

Remove-Item -Recurse -Force 'HKCU:\Software\TDisplay' -ErrorAction SilentlyContinue
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name TDisplay -ErrorAction SilentlyContinue

Write-Host ''
Write-Host 'Done. This PC now looks like one that has never seen TDisplay.'
Write-Host 'Next, double-click dist\TDisplaySetup.exe.'
Read-Host 'Press Enter to close this window'
