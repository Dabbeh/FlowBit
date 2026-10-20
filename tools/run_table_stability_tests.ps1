# Builds and runs test_table_stability, which guards the column-heading shift.
#
# The user diagnosed this one directly: when the panel opened, the table briefly
# showed its own vertical scrollbar and then it vanished. A scrollbar occupies
# width, so its appearance narrowed the table viewport and its disappearance widened
# it again - sliding the headings sideways and re-measuring the section. Worse when
# maximized, because more rows fit.
#
# The fix reserves the space permanently (ScrollBarAlwaysOn). The test asserts the
# invariant that makes the shift impossible, at two window sizes:
#   * the table viewport width never changes
#   * the column width total never changes
#   * the rightmost column never slides
#   * the scrollbar never VANISHES while the table is shown (its one-time appearance
#     when the table first becomes visible is expected and not counted)
#
# It also fails loudly if the table is never visible, because a hidden table has no
# geometry to measure and the comparisons would then pass without testing anything.

$ErrorActionPreference = 'Stop'
$srcRoot = Split-Path -Parent $PSScriptRoot

$outDir = Join-Path $srcRoot 'build\tests\stability'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$moc = 'C:\Qt\6.10.0\mingw_64\bin\moc.exe'
$gpp = 'C:\Qt\Tools\mingw1310_64\bin\g++.exe'
$qtInc = 'C:\Qt\6.10.0\mingw_64\include'
$qtLib = 'C:\Qt\6.10.0\mingw_64\lib'

foreach ($h in @('appactivitypanel.h', 'appmonitor.h', 'busydots.h')) {
    & $moc "-I$srcRoot\src" "-I$qtInc" "$srcRoot\src\$h" -o (Join-Path $outDir ("moc_" + ($h -replace '\.h$', '.cpp')))
}

$sources = @(
    "$srcRoot\tools\test_table_stability.cpp",
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

$exe = Join-Path $outDir 'test_table_stability.exe'
& $gpp -std=c++17 -O1 -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    "-I$qtInc" "-I$qtInc\QtCore" "-I$qtInc\QtGui" "-I$qtInc\QtWidgets" `
    "-I$srcRoot\src" "-I$srcRoot\third_party\WinDivert\WinDivert-2.2.2-A\include" `
    $sources `
    "-L$qtLib" "-L$srcRoot\third_party\WinDivert\WinDivert-2.2.2-A\x64" `
    -lQt6Widgets -lQt6Gui -lQt6Core `
    -lwindivert -ladvapi32 -ldwmapi -ltdh -liphlpapi -lws2_32 -lpsapi -lwinmm `
    -o $exe

if (-not (Test-Path $exe)) { Write-Host "test_table_stability failed to compile"; exit 1 }

# Run beside the deployed Qt DLLs; a bare exe in build\tests cannot resolve them.
$appDir = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Flowbit'
if (Test-Path (Join-Path $appDir 'Qt6Core.dll')) {
    $staged = Join-Path $appDir 'test_table_stability.exe'
    Copy-Item $exe $staged -Force
    & $staged
    $rc = $LASTEXITCODE
    Remove-Item $staged -Force -ErrorAction SilentlyContinue
    exit $rc
}

& $exe
exit $LASTEXITCODE
