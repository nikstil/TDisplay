# Tests the cable between the laptop and the tablet: link speed, charging power, and how fast and
# how reliably data really moves in each direction. Run it with nothing else using the tablet.
param(
    [int]$SizeMB = 200,
    [int]$Rounds = 3
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$adb = "$root\dist\adb\adb.exe"
$adbArgs = @('-P', '15037')

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class UsbPort {
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle device, uint code, byte[] input, int inputSize, byte[] output, int outputSize, out int returned, IntPtr overlapped);

    // Speed: 0 low, 1 full, 2 high (USB 2), 3 SuperSpeed or faster (USB 3).
    public static int Speed, BcdUsb;
    public static bool PortUsb3, DeviceUsb3Capable, Operating10G, Capable10G;

    public static void Query(string hubPath, int port) {
        using (var hub = CreateFileW(hubPath, 0x40000000, 2, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (hub.IsInvalid) throw new InvalidOperationException("cannot open hub, error " + Marshal.GetLastWin32Error());
            int returned;
            var info = new byte[512];
            BitConverter.GetBytes(port).CopyTo(info, 0);
            if (!DeviceIoControl(hub, 0x220448, info, info.Length, info, info.Length, out returned, IntPtr.Zero))
                throw new InvalidOperationException("cannot read port, error " + Marshal.GetLastWin32Error());
            Speed = info[23];
            BcdUsb = BitConverter.ToUInt16(info, 6);
            var v2 = new byte[16];
            BitConverter.GetBytes(port).CopyTo(v2, 0);
            BitConverter.GetBytes(16).CopyTo(v2, 4);
            BitConverter.GetBytes(7).CopyTo(v2, 8);
            if (DeviceIoControl(hub, 0x22045C, v2, 16, v2, 16, out returned, IntPtr.Zero)) {
                uint protocols = BitConverter.ToUInt32(v2, 8), flags = BitConverter.ToUInt32(v2, 12);
                // The first query can say high speed for a device that's really on USB 3, so this flag decides.
                if ((flags & 1) != 0) Speed = 3;
                PortUsb3 = (protocols & 4) != 0;
                DeviceUsb3Capable = (flags & 2) != 0;
                Operating10G = (flags & 4) != 0;
                Capable10G = (flags & 8) != 0;
            }
        }
    }
}
'@

$serial = ((& $adb @adbArgs devices | Select-Object -Skip 1 | Where-Object { $_ -match '\tdevice$' } | Select-Object -First 1) -split '\t')[0]
if (-not $serial) { throw 'No tablet found. Is it plugged in and is TDisplay running?' }

Write-Host "=== Link ==="
$device = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match "^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}\\$serial$" } | Select-Object -First 1
$parent = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_Parent).Data
$location = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_LocationInfo).Data
$arrivalBefore = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_LastArrivalDate).Data
$port = [int]([regex]::Match($location, 'Port_#(\d+)').Groups[1].Value)
$hubPath = '\\.\' + ($parent -replace '\\', '#') + '#{f18a0e88-c30c-11d0-8815-00a0c906bed8}'
[UsbPort]::Query($hubPath, $port)
$linkName = switch ([UsbPort]::Speed) {
    3 { if ([UsbPort]::Operating10G) { 'USB 3 at 10 Gbps' } else { 'USB 3 at 5 Gbps' } }
    2 { 'USB 2 at 480 Mbps' }
    1 { 'USB 1 at 12 Mbps' }
    default { 'USB at low speed' }
}
Write-Host "Connected at:        $linkName"
Write-Host "Tablet supports:     $(if ([UsbPort]::Capable10G) { 'USB 3 at 10 Gbps' } elseif ([UsbPort]::DeviceUsb3Capable) { 'USB 3 at 5 Gbps or better' } else { 'USB 2 only (as reported)' })"
Write-Host "USB device version:  $('0x{0:X4}' -f [UsbPort]::BcdUsb)   Hub: $parent   $location"

Write-Host "`n=== Charging ==="
$battery = (& $adb @adbArgs shell dumpsys battery) -join "`n"
$current = [double]([regex]::Match($battery, 'Max charging current: (\d+)').Groups[1].Value) / 1000000
$voltage = [double]([regex]::Match($battery, 'Max charging voltage: (\d+)').Groups[1].Value) / 1000000
$source = if ($battery -match 'AC powered: true') { 'fast charger (AC)' } elseif ($battery -match 'USB powered: true') { 'USB' } else { 'none' }
$statusNames = @{ '1' = 'unknown'; '2' = 'charging'; '3' = 'discharging'; '4' = 'not charging'; '5' = 'full' }
$status = $statusNames[[regex]::Match($battery, 'status: (\d)').Groups[1].Value]
$level = [regex]::Match($battery, '\blevel: (\d+)').Groups[1].Value
Write-Host ("Source: {0}, limit {1:n2} A at {2:n1} V = {3:n1} W. Battery {4}%, {5}." -f $source, $current, $voltage, ($current * $voltage), $level, $status)

Write-Host "`n=== Transfer test: $Rounds rounds of $SizeMB MB, random data, checksums compared ==="
$temp = Join-Path $env:TEMP "tdisplay-cable-test.bin"
$back = Join-Path $env:TEMP "tdisplay-cable-test.back"
$remote = '/data/local/tmp/tdisplay-cable-test.bin'
$bytes = New-Object byte[] (1MB)
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
$stream = [IO.File]::Create($temp)
for ($i = 0; $i -lt $SizeMB; $i++) { $rng.GetBytes($bytes); $stream.Write($bytes, 0, $bytes.Length) }
$stream.Close()
$localHash = (Get-FileHash $temp -Algorithm MD5).Hash.ToLower()

$failures = 0
$pushRates = @()
$pullRates = @()
# adb writes progress to stderr, which PowerShell would otherwise treat as a failure.
$ErrorActionPreference = 'Continue'
for ($round = 1; $round -le $Rounds; $round++) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $adb @adbArgs push $temp $remote *> $null
    if ($LASTEXITCODE -ne 0) { $failures++; Write-Host "  round $round`: send failed"; continue }
    $pushRate = $SizeMB / $watch.Elapsed.TotalSeconds
    $remoteHash = ((& $adb @adbArgs shell md5sum $remote) -split '\s+')[0]

    $watch.Restart()
    & $adb @adbArgs pull $remote $back *> $null
    if ($LASTEXITCODE -ne 0) { $failures++; Write-Host "  round $round`: receive failed"; continue }
    $pullRate = $SizeMB / $watch.Elapsed.TotalSeconds
    $backHash = (Get-FileHash $back -Algorithm MD5).Hash.ToLower()

    $ok = ($remoteHash -eq $localHash) -and ($backHash -eq $localHash)
    if (-not $ok) { $failures++ }
    $pushRates += $pushRate
    $pullRates += $pullRate
    Write-Host ("  round {0}: to tablet {1,6:n1} MB/s, from tablet {2,6:n1} MB/s, data {3}" -f $round, $pushRate, $pullRate, $(if ($ok) { 'intact' } else { 'CORRUPTED' }))
}
& $adb @adbArgs shell rm -f $remote | Out-Null
Remove-Item -Force $temp, $back -ErrorAction SilentlyContinue

$arrivalAfter = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_LastArrivalDate).Data
Write-Host "`n=== Verdict ==="
if ($arrivalAfter -ne $arrivalBefore) { Write-Host 'USB DROPPED during the test: the tablet disconnected and came back.' }
elseif ($failures -gt 0) { Write-Host "$failures of $Rounds rounds FAILED." }
else { Write-Host 'No drops and no corrupted data.' }
if ($pushRates.Count) {
    $best = ($pushRates | Measure-Object -Maximum).Maximum
    # 80 Mbps of video is 10 MB/s; twice that leaves room for bursts.
    Write-Host ("Best speed to the tablet: {0:n0} MB/s, which is {1:n0} Mbps. The video stream uses up to 80 Mbps." -f $best, ($best * 8))
    Write-Host ("A large 300 KB frame takes about {0:n1} ms to cross this cable." -f (0.3 / $best * 1000))
}
if ([UsbPort]::Speed -lt 3 -and [UsbPort]::DeviceUsb3Capable) {
    Write-Host 'The tablet can go faster, so this cable or this laptop port is limiting it to USB 2.'
}
