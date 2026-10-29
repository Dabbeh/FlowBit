# Builds and runs test_animation_curve, which guards the Show More reveal.
#
# The test asserts the SHAPE of the motion, not just that it runs:
#   * monotonic (the panel never shrinks mid-reveal)
#   * starts slowly (an ease-in-out curve, not the ease-out step that was reported
#     as "it pushes")
#   * settles at the end
#   * nothing else on screen moves - no horizontal shift, no moving top edge
#
# It also guards the defect found while building it: the reveal target was measured
# while the panel was collapsed, where a hidden table still contributes to the size
# hint. The panel reported 514px but settled at 188px, so the tween overshot by
# 326px and the correction snapped back. A large step at the end would reappear if
# that measurement regressed.

$ErrorActionPreference = 'Stop'
$srcRoot = Split-Path -Parent $PSScriptRoot

$outDir = Join-Path $srcRoot 'build\tests\animcurve'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$moc = 'C:\Qt\6.10.0\mingw_64\bin\moc.exe'
$gpp = 'C:\Qt\Tools\mingw1310_64\bin\g++.exe'
$qtInc = 'C:\Qt\6.10.0\mingw_64\include'
$qtLib = 'C:\Qt\6.10.0\mingw_64\lib'
$qtBin = 'C:\Qt\6.10.0\mingw_64\bin'
$mingwBin = 'C:\Qt\Tools\mingw1310_64\bin'
$env:Path = "$mingwBin;$qtBin;$env:Path"
$env:QT_QPA_PLATFORM = 'offscreen'

foreach ($h in @('appactivitypanel.h', 'appmonitor.h', 'busydots.h')) {
    & $moc "-I$srcRoot\src" "-I$qtInc" "$srcRoot\src\$h" -o (Join-Path $outDir ("moc_" + ($h -replace '\.h$', '.cpp')))
}

$sources = @(
    "$srcRoot\tools\test_animation_curve.cpp",
    "$srcRoot\src\appactivitypanel.cpp",
    "$srcRoot\src\appmonitor.cpp",
    "$srcRoot\src\etwclass.cpp",
    "$srcRoot\src\appnames.cpp",
    "$srcRoot\src\theme.cpp",
    "$srcRoot\src\checkmark.cpp",
    "$srcRoot\src\units.cpp",
    "$srcRoot\src\busydots.cpp",
    "$srcRoot\src\navicons.cpp",
    (Join-Path $outDir 'moc_appactivitypanel.cpp'),
    (Join-Path $outDir 'moc_appmonitor.cpp'),
    (Join-Path $outDir 'moc_busydots.cpp')
)

$exe = Join-Path $outDir 'test_animation_curve.exe'
& $gpp -std=c++17 -O1 -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    "-I$qtInc" "-I$qtInc\QtCore" "-I$qtInc\QtGui" "-I$qtInc\QtWidgets" `
    "-I$srcRoot\src" "-I$srcRoot\third_party\WinDivert\WinDivert-2.2.2-A\include" `
    $sources `
    "-L$qtLib" -lQt6Widgets -lQt6Gui -lQt6Core `
    "-L$srcRoot\third_party\WinDivert\WinDivert-2.2.2-A\x64" `
    -lwindivert -ladvapi32 -ldwmapi -ltdh -liphlpapi -lws2_32 -lpsapi -lwinmm `
    -o $exe
if ($LASTEXITCODE -ne 0) { throw 'test_animation_curve failed to compile' }

if (-not (Test-Path $exe)) { Write-Host "test_animation_curve failed to compile"; exit 1 }

# Stage Qt's offscreen runtime beside the executable so the test is repeatable
# and never opens a visible window.
foreach ($dll in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll')) {
    Copy-Item (Join-Path $qtBin $dll) $outDir -Force
}
foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item (Join-Path $mingwBin $dll) $outDir -Force
}
$platformDir = Join-Path $outDir 'platforms'
New-Item -ItemType Directory -Force -Path $platformDir | Out-Null
Copy-Item 'C:\Qt\6.10.0\mingw_64\plugins\platforms\qoffscreen.dll' $platformDir -Force
Copy-Item (Join-Path $srcRoot 'third_party\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll') $outDir -Force

Push-Location $outDir
try { & $exe; $rc = $LASTEXITCODE } finally { Pop-Location }
exit $rc
