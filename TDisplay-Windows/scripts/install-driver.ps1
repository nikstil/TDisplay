# Installs (or with -Uninstall removes) the virtual monitor driver. Asks for elevation itself.
# -PauseSuperDisplay also stops SuperDisplay's service until the next reboot, because it holds the
# tablet's USB connection. It does not change or uninstall SuperDisplay.
param(
    [switch]$Uninstall,
    [switch]$PauseSuperDisplay
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$logFile = "$root\build\driver-install.log"

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Uninstall) { $arguments += '-Uninstall' }
    if ($PauseSuperDisplay) { $arguments += '-PauseSuperDisplay' }
    $process = Start-Process powershell -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    if (Test-Path $logFile) { Get-Content $logFile }
    exit $process.ExitCode
}

New-Item -ItemType Directory -Force (Split-Path -Parent $logFile) | Out-Null
Start-Transcript -Path $logFile -Force | Out-Null

$hardwareId = 'Root\MttVDD'
$displayClass = [Guid]'4d36e968-e325-11ce-bfc1-08002be10318'

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class RootDevice {
    const uint DIF_REGISTERDEVICE = 0x19;
    const uint DICD_GENERATE_ID = 1;
    const uint SPDRP_HARDWAREID = 1;
    const uint INSTALLFLAG_FORCE = 1;

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid classGuid, IntPtr parent);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid classGuid, string description, IntPtr parent, uint flags, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, uint property, byte[] buffer, uint size);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(uint function, IntPtr set, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr parent, string hardwareId, string infPath, uint flags, out bool reboot);

    public static void Create(string hardwareId, Guid classGuid, string name) {
        IntPtr set = SetupDiCreateDeviceInfoList(ref classGuid, IntPtr.Zero);
        if (set == new IntPtr(-1)) throw new Win32Exception();
        try {
            var data = new SP_DEVINFO_DATA();
            data.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            if (!SetupDiCreateDeviceInfoW(set, name, ref classGuid, name, IntPtr.Zero, DICD_GENERATE_ID, ref data)) throw new Win32Exception();
            byte[] id = Encoding.Unicode.GetBytes(hardwareId + "\0\0");
            if (!SetupDiSetDeviceRegistryPropertyW(set, ref data, SPDRP_HARDWAREID, id, (uint)id.Length)) throw new Win32Exception();
            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref data)) throw new Win32Exception();
        } finally {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    public static bool Install(string hardwareId, string infPath) {
        bool reboot;
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hardwareId, infPath, INSTALLFLAG_FORCE, out reboot)) throw new Win32Exception();
        return reboot;
    }
}
'@

function Find-DriverDevices {
    Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Where-Object {
        $ids = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_HardwareIds -ErrorAction SilentlyContinue).Data
        $ids -contains $hardwareId
    }
}

$failed = $false
try {
    if ($PauseSuperDisplay) {
        $service = Get-Service SuperDisplay -ErrorAction SilentlyContinue
        if ($service -and $service.Status -eq 'Running') {
            Write-Host 'Pausing SuperDisplay until the next reboot'
            Stop-Service SuperDisplay -Force
        }
        # Its adb server can outlive the service and would keep hold of the tablet.
        Get-CimInstance Win32_Process -Filter "Name='adb.exe'" | Where-Object { $_.ExecutablePath -like '*\SuperDisplay\*' } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    }

    if ($Uninstall) {
        foreach ($device in @(Find-DriverDevices)) {
            Write-Host "Removing $($device.InstanceId)"
            & pnputil.exe /remove-device $device.InstanceId
        }
        $packages = (& pnputil.exe /enum-drivers | Out-String) -split "(\r?\n){2,}" | Where-Object { $_ -match 'mttvdd\.inf' }
        foreach ($package in $packages) {
            if ($package -match '(oem\d+\.inf)') {
                Write-Host "Deleting driver package $($Matches[1])"
                & pnputil.exe /delete-driver $Matches[1] /uninstall /force
            }
        }
        Write-Host 'Driver removed.'
    } else {
        # The driver reads its modes from this fixed location when it starts.
        New-Item -ItemType Directory -Force 'C:\VirtualDisplayDriver' | Out-Null
        Copy-Item -Force "$root\driver\vdd_settings.xml" 'C:\VirtualDisplayDriver\vdd_settings.xml'

        $package = "$root\driver\package\VirtualDisplayDriver"
        $signature = Get-AuthenticodeSignature "$package\mttvdd.cat"
        if ($signature.Status -ne 'Valid') { throw "Driver signature is $($signature.Status); not installing." }
        Write-Host "Driver signature: $($signature.Status), $($signature.SignerCertificate.Subject)"

        if (@(Find-DriverDevices).Count -eq 0) {
            Write-Host 'Creating the virtual display device'
            [RootDevice]::Create($hardwareId, $displayClass, 'Display')
        }
        Write-Host 'Installing the driver (Windows may ask you to confirm the publisher)'
        $reboot = [RootDevice]::Install($hardwareId, "$package\MttVDD.inf")
        if ($reboot) { Write-Host 'Windows says a reboot is needed.' }
        Find-DriverDevices | Format-Table Status, FriendlyName, InstanceId -AutoSize | Out-String | Write-Host
        Write-Host 'Driver installed.'
    }
} catch {
    Write-Host "FAILED: $($_.Exception.Message)"
    $failed = $true
}
Stop-Transcript | Out-Null
if ($failed) { exit 1 }
