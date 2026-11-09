# Builds the laptop program and the tablet app into dist\.
param(
    [switch]$HostOnly,
    [switch]$AppOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tools = "$root\tools"
$dist = "$root\dist"
$build = "$root\build"
New-Item -ItemType Directory -Force $dist, $build | Out-Null

function Run($exe) {
    # Tools print notes on stderr; with output captured, Windows PowerShell would treat those as errors and stop.
    $ErrorActionPreference = 'Continue'
    & $exe @args
    if ($LASTEXITCODE -ne 0) { throw "failed: $exe $args" }
}

# Version details for the program's and the setup's Properties > Details, from the version the tablet app uses and the
# copyright line in LICENSE, so there's one place to change each.
function WriteVersionHeader {
    $protocolText = Get-Content -Raw "$root\host\src\protocol.h"
    $protocol = [int][regex]::Match($protocolText, 'kVersion = (\d+)').Groups[1].Value
    $revision = [int][regex]::Match($protocolText, 'kAppRevision = (\d+)').Groups[1].Value
    # LICENSE is in this folder here, and one folder up in the GitHub repository, which holds the Mac version too.
    $license = @("$root\LICENSE", "$(Split-Path -Parent $root)\LICENSE") | Where-Object { Test-Path $_ } | Select-Object -First 1
    $copyright = 'Copyright (c) the TDisplay contributors'
    if ($license) { $copyright = (Get-Content $license | Where-Object { $_ -like 'Copyright*' } | Select-Object -First 1) -replace '"', "'" }
    Set-Content -Encoding ascii "$build\version.h" @(
        '// Made by scripts\build.ps1 from host\src\protocol.h and LICENSE.',
        "#define TD_VERSION 1,$protocol,$revision,0",
        "#define TD_VERSION_TEXT ""1.$protocol.$revision""",
        "#define TD_COPYRIGHT ""$copyright. MIT license."""
    )
}

function BuildHost {
    # A running copy locks the file.
    Get-Process tdisplay-host -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    $cxx = "$tools\llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe"
    $rc = "$tools\llvm-mingw\bin\x86_64-w64-mingw32-windres.exe"
    $obj = "$build\host"
    New-Item -ItemType Directory -Force $obj | Out-Null
    Push-Location "$root\host\res"
    Run $rc app.rc -I "$build" -O coff -o "$obj\app.res.o"
    Pop-Location
    $flags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
        '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
        '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A00000F',
        "-I$root\host\third_party")
    $objects = @("$obj\app.res.o")
    $sources = Get-ChildItem "$root\host\src\*.cpp"
    $headers = Get-ChildItem "$root\host\src\*.h" | Sort-Object LastWriteTime | Select-Object -Last 1
    foreach ($source in $sources) {
        $output = "$obj\$($source.BaseName).o"
        $objects += $output
        if ((Test-Path $output) -and (Get-Item $output).LastWriteTime -gt $source.LastWriteTime -and (Get-Item $output).LastWriteTime -gt $headers.LastWriteTime) { continue }
        Write-Host "  $($source.Name)"
        Run $cxx @flags -c $source.FullName -o $output
    }
    Run $cxx @objects -o "$dist\tdisplay-host.exe" -municode -mwindows -static -s `
        -ld3d11 -ldxgi -lws2_32 -lshell32 -lole32 -luuid -lwinmm -lavrt -lgdi32 -luser32 -ladvapi32 -lcfgmgr32
    # Test tools share the program's sources, so they're rebuilt when those change too.
    $shared = 'capture', 'display', 'encoder', 'mfencoder', 'util' | ForEach-Object { "$obj\$_.o" }
    $encoders = 'encoder', 'mfencoder', 'util' | ForEach-Object { "$obj\$_.o" }
    $toolFlags = $flags + "-I$root\host\src"
    $toolIsCurrent = { param($exe, $sources) (Test-Path $exe) -and -not ($sources | Where-Object { (Get-Item $_).LastWriteTime -gt (Get-Item $exe).LastWriteTime }) }
    if (-not (& $toolIsCurrent "$build\tdisplay-bench.exe" (@("$root\host\tools\bench.cpp") + $shared))) {
        Run $cxx @toolFlags "$root\host\tools\bench.cpp" @shared "$obj\app.res.o" -o "$build\tdisplay-bench.exe" -municode -static -s `
            -ld3d11 -ldxgi -lshell32 -lole32 -luuid -lwinmm -lavrt -luser32
    }
    if (-not (& $toolIsCurrent "$build\tdisplay-enctest.exe" (@("$root\host\tools\enctest.cpp") + $encoders))) {
        Run $cxx @toolFlags "$root\host\tools\enctest.cpp" @encoders "$obj\app.res.o" -o "$build\tdisplay-enctest.exe" -municode -static -s `
            -ld3d11 -ldxgi -lshell32 -lole32 -luuid -lavrt -lgdi32 -luser32
    }
    if (-not (& $toolIsCurrent "$build\tdisplay-test.exe" @("$root\host\tools\testwindow.cpp"))) {
        Run $cxx @toolFlags "$root\host\tools\testwindow.cpp" "$obj\display.o" "$obj\util.o" "$obj\app.res.o" -o "$build\tdisplay-test.exe" -municode -mwindows -static -s `
            -ld3d11 -ldxgi -lshell32 -lole32 -luuid -luser32
    }
    if (-not (& $toolIsCurrent "$build\tdisplay-pentest.exe" @("$root\host\tools\pentest.cpp", "$obj\pen.o"))) {
        Run $cxx @toolFlags "$root\host\tools\pentest.cpp" "$obj\display.o" "$obj\pen.o" "$obj\util.o" "$obj\app.res.o" -o "$build\tdisplay-pentest.exe" -municode -static -s `
            -lshell32 -lole32 -luuid -luser32
    }
    $adb = "$dist\adb"
    New-Item -ItemType Directory -Force $adb | Out-Null
    foreach ($file in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll') {
        # The adb server keeps running between builds and locks these, and they rarely change.
        if (-not (Test-Path "$adb\$file")) { Copy-Item "$tools\android-sdk\platform-tools\$file" $adb }
    }
    Write-Host "Built $dist\tdisplay-host.exe"
}

function BuildApp {
    $env:JAVA_HOME = "$tools\jdk"
    $env:PATH = "$tools\jdk\bin;$env:PATH"
    $bt = "$tools\android-sdk\build-tools\35.0.0"
    $jar = "$tools\android-sdk\platforms\android-35\android.jar"
    $src = "$root\android"
    $out = "$build\app"
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force "$out\classes", "$out\dex", "$out\gen" | Out-Null

    # The version code comes from the protocol version and the app revision, so the laptop can spot a stale app.
    $protocolText = Get-Content -Raw "$root\host\src\protocol.h"
    $protocol = [int][regex]::Match($protocolText, 'kVersion = (\d+)').Groups[1].Value
    $revision = [int][regex]::Match($protocolText, 'kAppRevision = (\d+)').Groups[1].Value
    $version = $protocol * 100 + $revision
    Run "$bt\aapt2.exe" compile --dir "$src\res" -o "$out\res.zip"
    Run "$bt\aapt2.exe" link -o "$out\unsigned.apk" -I $jar --manifest "$src\AndroidManifest.xml" `
        --min-sdk-version 30 --target-sdk-version 35 --version-code $version --version-name "1.$protocol.$revision" --custom-package com.tdisplay --java "$out\gen" "$out\res.zip"
    $java = @(Get-ChildItem "$src\java" -Recurse -Filter *.java | ForEach-Object { $_.FullName }) +
        @(Get-ChildItem "$out\gen" -Recurse -Filter *.java | ForEach-Object { $_.FullName })
    Run "$tools\jdk\bin\javac.exe" '-nowarn' '-Xlint:none' --release 11 -classpath $jar -d "$out\classes" @java
    $classes = Get-ChildItem "$out\classes" -Recurse -Filter *.class | ForEach-Object { $_.FullName }
    # d8 is a batch file, and batch files mangle long argument lists, so pass classes in a file.
    Set-Content -Encoding ascii "$out\classes.txt" $classes
    Run "$bt\d8.bat" --release --lib $jar --min-api 30 --output "$out\dex" "@$out\classes.txt"
    Push-Location "$out\dex"
    Run "$bt\aapt.exe" add "$out\unsigned.apk" classes.dex
    Pop-Location
    Run "$bt\zipalign.exe" -f -p 4 "$out\unsigned.apk" "$out\aligned.apk"

    # A key private to this checkout. Android requires updates to use the same one.
    $key = "$root\android\signing.keystore"
    if (-not (Test-Path $key)) {
        Run "$tools\jdk\bin\keytool.exe" -genkeypair -keystore $key -storepass tdisplay -keypass tdisplay -alias tdisplay `
            -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=TDisplay' -storetype PKCS12
    }
    Run "$bt\apksigner.bat" sign --ks $key --ks-pass pass:tdisplay --ks-key-alias tdisplay --out "$dist\TDisplay.apk" "$out\aligned.apk"
    Write-Host "Built $dist\TDisplay.apk"
}

WriteVersionHeader
if (-not $AppOnly) { BuildHost }
if (-not $HostOnly) { BuildApp }
