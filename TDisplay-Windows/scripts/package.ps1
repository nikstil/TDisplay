# Builds everything and wraps it into dist\TDisplaySetup.exe: one file to give to other people.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tools = "$root\tools"
$dist = "$root\dist"
$obj = "$root\build\setup"
New-Item -ItemType Directory -Force $obj | Out-Null

function Run($exe) {
    # Tools print notes on stderr; with output captured, Windows PowerShell would treat those as errors and stop.
    $ErrorActionPreference = 'Continue'
    & $exe @args
    if ($LASTEXITCODE -ne 0) { throw "failed: $exe $args" }
}

& "$PSScriptRoot\build.ps1"

Push-Location "$root\setup"
Run "$tools\llvm-mingw\bin\x86_64-w64-mingw32-windres.exe" setup.rc -I "$root\build" -O coff -o "$obj\setup.res.o"
Pop-Location
Run "$tools\llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe" -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A00000F `
    "-I$root\host\src" "-I$root\host\third_party" "$root\setup\setup.cpp" "$obj\setup.res.o" -o "$dist\TDisplaySetup.exe" -municode -mwindows -static -s `
    -lsetupapi -lnewdev -lcrypt32 -ldxgi -lole32 -luuid -lshell32 -lcomctl32 -lws2_32 -ladvapi32 -luser32 -lgdi32

$size = (Get-Item "$dist\TDisplaySetup.exe").Length / 1MB
Write-Host ("Built $dist\TDisplaySetup.exe ({0:n1} MB)" -f $size)
