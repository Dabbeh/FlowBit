# Builds and runs the diagnostics suite.
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$qt    = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'
$out   = Join-Path $root 'build\tests'
$env:Path = "$mingw;$qt\bin;$env:Path"

New-Item -ItemType Directory -Force -Path $out | Out-Null

$mocOut = Join-Path $out 'moc'
New-Item -ItemType Directory -Force -Path $mocOut | Out-Null
& "$qt\bin\moc.exe" "-I$root\src" (Join-Path $root 'src\diagnosticspanel.h') `
    -o (Join-Path $mocOut 'moc_diagnosticspanel.cpp')
if ($LASTEXITCODE -ne 0) { throw 'moc failed' }
# The report reads the monitor's session state, so its moc is needed too.
& "$qt\bin\moc.exe" "-I$root\src" (Join-Path $root 'src\appmonitor.h') `
    -o (Join-Path $mocOut 'moc_appmonitor.cpp')
if ($LASTEXITCODE -ne 0) { throw 'moc failed for appmonitor.h' }

$exe = Join-Path $out 'test_diagnostics.exe'
Write-Host '==> building test_diagnostics' -ForegroundColor Cyan
& "$mingw\g++.exe" -std=gnu++17 -O1 -DWIN32 -DUNICODE -D_UNICODE `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -DNTDDI_VERSION=0x06010000 -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 `
    "-I$root\src" `
    "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" "-I$qt\include\QtWidgets" `
    "-I$qt\mkspecs\win32-g++" `
    (Join-Path $root 'tools\test_diagnostics.cpp') `
    (Join-Path $root 'src\diagnostics.cpp') `
    (Join-Path $root 'src\diagnosticspanel.cpp') `
    (Join-Path $root 'src\appmonitor.cpp') `
    (Join-Path $root 'src\etwclass.cpp') `
    (Join-Path $root 'src\appnames.cpp') `
    (Join-Path $root 'src\theme.cpp') `
    (Join-Path $root 'src\checkmark.cpp') `
    (Join-Path $root 'src\units.cpp') `
    (Join-Path $mocOut 'moc_diagnosticspanel.cpp') `
    (Join-Path $mocOut 'moc_appmonitor.cpp') `
    -o $exe "-L$qt\lib" -lQt6Widgets -lQt6Gui -lQt6Core `
    -ladvapi32 -ldwmapi -ltdh -lwintrust -lcrypt32 -liphlpapi -lws2_32 -lmingw32
if ($LASTEXITCODE -ne 0) { throw 'test_diagnostics failed to compile' }

# The report reads the driver files beside the executable, so they must be present
# for the readings to be meaningful.
Copy-Item (Join-Path $root 'third_party\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll') $out -Force
Copy-Item (Join-Path $root 'third_party\WinDivert\WinDivert-2.2.2-A\x64\WinDivert64.sys') $out -Force

Write-Host '==> running' -ForegroundColor Cyan
$env:QT_QPA_PLATFORM = 'offscreen'
Push-Location $out
& $exe
$rc = $LASTEXITCODE
Pop-Location

if ($rc -ne 0) { throw "test_diagnostics reported failures (exit $rc)" }
Write-Host ''
Write-Host 'test_diagnostics passed.' -ForegroundColor Green
