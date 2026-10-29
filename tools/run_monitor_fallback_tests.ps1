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
$qtBin = 'C:\Qt\6.10.0\mingw_64\bin'
$mingwBin = 'C:\Qt\Tools\mingw1310_64\bin'
$env:Path = "$mingwBin;$qtBin;$env:Path"

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
if ($LASTEXITCODE -ne 0) { throw 'test_monitor_fallback failed to compile' }

if (-not (Test-Path $exe)) { Write-Host "test_monitor_fallback failed to compile"; exit 1 }

# Stage Qt's runtime beside the test. Run the test unelevated and in place.
foreach ($dll in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll')) {
    Copy-Item (Join-Path $qtBin $dll) $outDir -Force
}
foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item (Join-Path $mingwBin $dll) $outDir -Force
}
Push-Location $outDir
try { & $exe; $rc = $LASTEXITCODE } finally { Pop-Location }
exit $rc
