# Downloads the portable build tools into tools\ and the vendored files they need.
# Nothing here is installed system-wide; deleting tools\ removes all of it.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root 'tools'
$cache = Join-Path $tools '_downloads'
New-Item -ItemType Directory -Force $cache | Out-Null

function Fetch($url, $name) {
    $file = Join-Path $cache $name
    if (-not (Test-Path $file)) {
        Write-Host "Downloading $name"
        & curl.exe -L --fail --retry 3 --retry-delay 5 --silent --show-error -o "$file.part" $url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
        Move-Item -Force "$file.part" $file
    }
    return $file
}

# Unpacks a zip that has one top-level folder and moves that folder to $dest.
function Unpack($zip, $dest) {
    if (Test-Path $dest) { return }
    $tmp = "$dest.unpack"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    Write-Host "Unpacking $(Split-Path -Leaf $zip)"
    & tar.exe -xf $zip -C $tmp
    if ($LASTEXITCODE -ne 0) { throw "unpack failed: $zip" }
    $inner = @(Get-ChildItem $tmp)
    if ($inner.Count -ne 1) { throw "expected one top-level folder in $zip" }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Move-Item $inner[0].FullName $dest
    Remove-Item -Recurse -Force $tmp
}

$google = 'https://dl.google.com/android/repository'
Unpack (Fetch "$google/platform-tools-latest-windows.zip" 'platform-tools.zip') "$tools\android-sdk\platform-tools"
Unpack (Fetch "$google/platform-35_r02.zip" 'platform-35_r02.zip') "$tools\android-sdk\platforms\android-35"
Unpack (Fetch "$google/build-tools_r35_windows.zip" 'build-tools_r35_windows.zip') "$tools\android-sdk\build-tools\35.0.0"
Unpack (Fetch 'https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.13%2B9/OpenJDK21U-jdk_x64_windows_hotspot_21.0.13_9.zip' 'jdk21.zip') "$tools\jdk"
Unpack (Fetch 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-ucrt-x86_64.zip' 'llvm-mingw.zip') "$tools\llvm-mingw"

$header = "$root\host\third_party\nvEncodeAPI.h"
if (-not (Test-Path $header)) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $header) | Out-Null
    & curl.exe -L --fail --silent --show-error -o $header 'https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/n13.0.19.0/include/ffnvcodec/nvEncodeAPI.h'
    if ($LASTEXITCODE -ne 0) { throw 'nvEncodeAPI.h download failed' }
}

$driverDir = "$root\driver\package"
if (-not (Test-Path $driverDir)) {
    $zip = Fetch 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip' 'vdd-driver-25.7.23.zip'
    New-Item -ItemType Directory -Force $driverDir | Out-Null
    & tar.exe -xf $zip -C $driverDir
    if ($LASTEXITCODE -ne 0) { throw 'driver unpack failed' }
}

Write-Output 'Toolchain ready.'
