# Builds the per-application monitoring self-test.
#
# The tool must be RUN ELEVATED - creating an ETW trace session requires
# Administrator rights. This script only builds it.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_selftest.ps1
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$qt    = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'
$env:Path = "$mingw;$qt\bin;$env:Path"

$out = Join-Path $root 'build\selftest'
New-Item -ItemType Directory -Force -Path $out | Out-Null

Write-Host '==> moc for the monitor' -ForegroundColor Cyan
$mocOut = Join-Path $out 'moc'
New-Item -ItemType Directory -Force -Path $mocOut | Out-Null
& "$qt\bin\moc.exe" "-I$root\src" (Join-Path $root 'src\appmonitor.h') `
    -o (Join-Path $mocOut 'moc_appmonitor.cpp')
if ($LASTEXITCODE -ne 0) { throw 'moc failed' }

Write-Host '==> building appmon_selftest' -ForegroundColor Cyan
$exe = Join-Path $out 'appmon_selftest.exe'
& "$mingw\g++.exe" -std=gnu++17 -O1 -DWIN32 -DUNICODE -D_UNICODE `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -DNTDDI_VERSION=0x06010000 -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 `
    "-I$root\src" "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" `
    "-I$qt\include\QtWidgets" "-I$qt\mkspecs\win32-g++" `
    (Join-Path $root 'tools\appmon_selftest.cpp') `
    (Join-Path $root 'src\appmonitor.cpp') `
    (Join-Path $root 'src\appnames.cpp') `
    (Join-Path $root 'src\theme.cpp') `
    (Join-Path $root 'src\checkmark.cpp') `
    (Join-Path $mocOut 'moc_appmonitor.cpp') `
    -o $exe "-L$qt\lib" -lQt6Widgets -lQt6Gui -lQt6Core `
    -ladvapi32 -ldwmapi -ltdh -lwinhttp -lpsapi -lmingw32 -liphlpapi -lws2_32
if ($LASTEXITCODE -ne 0) { throw 'self-test failed to compile' }

Write-Host '==> staging the Qt runtime' -ForegroundColor Cyan
foreach ($d in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll')) {
    Copy-Item "$qt\bin\$d" $out -Force
}
foreach ($d in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item "$mingw\$d" $out -Force
}
$plat = Join-Path $out 'platforms'
New-Item -ItemType Directory -Force -Path $plat | Out-Null
Copy-Item "$qt\plugins\platforms\qwindows.dll" $plat -Force

Write-Host ''
Write-Host "Built: $exe" -ForegroundColor Green
Write-Host ''
Write-Host 'RUN THIS ELEVATED:' -ForegroundColor Yellow
Write-Host '  Right-click appmon_selftest.exe -> Run as administrator'
Write-Host '  or from an elevated PowerShell window:'
Write-Host "     & '$exe' 15"
Write-Host ''
Write-Host 'It generates real download traffic and reports which applications it' -ForegroundColor Gray
Write-Host 'attributed it to. Output is also written to appmon_selftest.log.' -ForegroundColor Gray
