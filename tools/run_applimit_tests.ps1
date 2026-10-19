# Builds and runs the per-application limiter suite.
#
# Covers the decision logic and persistence. Actual enforcement needs Administrator
# rights and is verified separately by the user - see docs/app-limiter-findings.md.
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$qt    = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'
$wd    = "$root\third_party\WinDivert\WinDivert-2.2.2-A"
$out   = Join-Path $root 'build\tests'
$env:Path = "$mingw;$qt\bin;$env:Path"

New-Item -ItemType Directory -Force -Path $out | Out-Null

# The engines are QObjects, so they need moc.
$mocOut = Join-Path $out 'moc'
New-Item -ItemType Directory -Force -Path $mocOut | Out-Null
& "$qt\bin\moc.exe" "-I$root\src" "-I$wd\include" (Join-Path $root 'src\applimitengine.h') `
    -o (Join-Path $mocOut 'moc_applimitengine.cpp')
if ($LASTEXITCODE -ne 0) { throw 'moc failed for applimitengine.h' }
& "$qt\bin\moc.exe" "-I$root\src" "-I$wd\include" (Join-Path $root 'src\throttleengine.h') `
    -o (Join-Path $mocOut 'moc_throttleengine.cpp')
if ($LASTEXITCODE -ne 0) { throw 'moc failed for throttleengine.h' }
& "$qt\bin\moc.exe" "-I$root\src" (Join-Path $root 'src\settingsstore.h') `
    -o (Join-Path $mocOut 'moc_settingsstore.cpp')
if ($LASTEXITCODE -ne 0) { throw 'moc failed for settingsstore.h' }

$sources = @(
    "$root\tools\test_applimit.cpp"
    "$root\src\applimitengine.cpp"
    "$root\src\throttleengine.cpp"
    "$root\src\settingsstore.cpp"
    "$root\src\schedule.cpp"
    "$root\src\units.cpp"
    (Join-Path $mocOut 'moc_applimitengine.cpp')
    (Join-Path $mocOut 'moc_throttleengine.cpp')
    (Join-Path $mocOut 'moc_settingsstore.cpp')
)

$exe = Join-Path $out 'test_applimit.exe'
Write-Host '==> building test_applimit' -ForegroundColor Cyan
& "$mingw\g++.exe" -std=gnu++17 -O1 -DWIN32 -DUNICODE -D_UNICODE `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -DNTDDI_VERSION=0x06010000 -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 `
    "-I$root\src" "-I$wd\include" `
    "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" "-I$qt\include\QtWidgets" `
    "-I$qt\mkspecs\win32-g++" `
    @sources -o $exe `
    "-L$wd\x64" -lwindivert "-L$qt\lib" -lQt6Widgets -lQt6Gui -lQt6Core `
    -liphlpapi -lws2_32 -lshell32 -lmingw32
if ($LASTEXITCODE -ne 0) { throw 'test_applimit failed to compile' }

# The driver DLL must sit beside the binary for WinDivertOpen to find it.
Copy-Item "$wd\x64\WinDivert.dll" $out -Force
Copy-Item "$wd\x64\WinDivert64.sys" $out -Force

$scratch = Join-Path $out 'applimit_scratch'
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }

Write-Host '==> running' -ForegroundColor Cyan
$env:QT_QPA_PLATFORM = 'offscreen'
Push-Location $out
& $exe
$rc = $LASTEXITCODE
Pop-Location

if ($rc -ne 0) { throw "test_applimit reported failures (exit $rc)" }
Write-Host ''
Write-Host 'test_applimit passed.' -ForegroundColor Green
