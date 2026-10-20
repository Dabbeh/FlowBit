# Builds and runs test_monitor_fallback.
#
# This test guards the invariant that cost two days: per-application monitoring must
# never report itself unavailable while a working data source exists. It is run
# UNELEVATED on purpose - that is the condition under which ETW session creation
# fails for this process, which exercises the fallback path naturally.

$ErrorActionPreference = 'Stop'
$srcRoot = Split-Path -Parent $PSScriptRoot

$outDir = Join-Path $srcRoot 'build\tests\fallback'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$moc = 'C:\Qt\6.10.0\mingw_64\bin\moc.exe'
$gpp = 'C:\Qt\Tools\mingw1310_64\bin\g++.exe'
$qtInc = 'C:\Qt\6.10.0\mingw_64\include'
$qtLib = 'C:\Qt\6.10.0\mingw_64\lib'

& $moc "-I$srcRoot\src" "-I$qtInc" "$srcRoot\src\appmonitor.h" -o (Join-Path $outDir 'moc_appmonitor.cpp')


$sources = @(
    "$srcRoot\tools\test_monitor_fallback.cpp",
    "$srcRoot\src\appmonitor.cpp",
    "$srcRoot\src\etwclass.cpp",
    "$srcRoot\src\appnames.cpp",
    "$srcRoot\src\theme.cpp",
    "$srcRoot\src\checkmark.cpp",
    "$srcRoot\src\units.cpp",
    (Join-Path $outDir 'moc_appmonitor.cpp')
)

$exe = Join-Path $outDir 'test_monitor_fallback.exe'
& $gpp -std=c++17 -O1 -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    "-I$qtInc" "-I$qtInc\QtCore" "-I$qtInc\QtGui" "-I$qtInc\QtWidgets" `
    "-I$srcRoot\src" "-I$srcRoot\third_party\WinDivert\WinDivert-2.2.2-A\include" `
    $sources `
    "-L$qtLib" -lQt6Widgets -lQt6Gui -lQt6Core `
    -ladvapi32 -ldwmapi -ltdh -liphlpapi -lws2_32 -lpsapi -luser32 -lgdi32 `
    -o $exe

if (-not (Test-Path $exe)) { Write-Host "test_monitor_fallback failed to compile"; exit 1 }

# Run beside the deployed Qt DLLs: a bare exe in build\tests cannot resolve them
# (0xC0000135), which would look like a test failure rather than a missing path.
$appDir = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Flowbit'
if (Test-Path (Join-Path $appDir 'Qt6Core.dll')) {
    $staged = Join-Path $appDir 'test_monitor_fallback.exe'
    Copy-Item $exe $staged -Force
    & $staged
    $rc = $LASTEXITCODE
    Remove-Item $staged -Force -ErrorAction SilentlyContinue
    exit $rc
}

& $exe
exit $LASTEXITCODE
