# Builds and runs Flowbit's unit tests (schedule rules, units, pacer math).
# These need no Administrator rights and touch no network.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$qt   = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'

$env:Path = "$mingw;$qt\bin;$env:Path"
$gpp = "$mingw\g++.exe"
$out = Join-Path $root 'build\tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$common = @(
    '-std=gnu++17', '-O2', '-DWIN32', '-DUNICODE', '-D_UNICODE',
    '-DNTDDI_VERSION=0x06010000', '-D_WIN32_WINNT=0x0601', '-DWINVER=0x0601',
    "-I$root\src", "-I$qt/include", "-I$qt/include/QtCore", "-I$qt/mkspecs/win32-g++"
)

# SpeedMonitor and the interface test need iphlpapi + the WinDivert headers are
# not required, but ws2_32/iphlpapi are.
$netLibs = @('-liphlpapi', '-lws2_32')

$moc = "$qt\bin\moc.exe"

# Runs moc over a header and returns the generated .cpp path, so tests can link
# Q_OBJECT classes (UsageTracker) without going through qmake.
function Invoke-Moc {
    param([string]$Header)

    $mocDir = Join-Path $out 'moc'
    New-Item -ItemType Directory -Force -Path $mocDir | Out-Null
    $base = [System.IO.Path]::GetFileNameWithoutExtension($Header)
    $target = Join-Path $mocDir "moc_$base.cpp"

    & $moc "-I$root\src" "-I$qt\include" "-I$qt\include\QtCore" $Header -o $target
    if ($LASTEXITCODE -ne 0) { throw "moc failed for $Header" }
    return $target
}

function Invoke-Test {
    param([string]$Name, [string[]]$Sources, [string[]]$ExtraLibs = @())

    Write-Host "==> building $Name" -ForegroundColor Cyan
    $exe = Join-Path $out "$Name.exe"
    $args = @($common) + $Sources + @('-o', $exe, "-L$qt/lib", '-lQt6Core', '-lmingw32') + $ExtraLibs
    & $gpp @args
    if ($LASTEXITCODE -ne 0) { throw "$Name failed to compile" }

    Write-Host "==> running $Name" -ForegroundColor Cyan
    Push-Location $out
    try {
        & $exe
        $rc = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($rc -ne 0) { throw "$Name reported failures" }
}

Invoke-Test -Name 'test_schedule' -Sources @(
    "$root\tools\test_schedule.cpp", "$root\src\schedule.cpp", "$root\src\units.cpp")

Invoke-Test -Name 'test_pacer' -Sources @("$root\tools\test_pacer.cpp")

Invoke-Test -Name 'test_usage' -Sources @(
    "$root\tools\test_usage.cpp", "$root\src\usagetracker.cpp",
    (Invoke-Moc "$root\src\usagetracker.h"))

# Rebrand safety: the NetValve -> Flowbit config-directory migration.
Invoke-Test -Name 'test_migration' -Sources @(
    "$root\tools\test_migration.cpp", "$root\src\settingsstore.cpp",
    "$root\src\schedule.cpp")

# Reads the live interface table, so it needs the networking libs.
Invoke-Test -Name 'test_interfaces' -Sources @("$root\tools\test_interfaces.cpp") `
    -ExtraLibs $netLibs

# Drives the real SpeedMonitor to confirm the rates it reports are plausible.
Invoke-Test -Name 'test_speedmonitor' -Sources @(
    "$root\tools\test_speedmonitor.cpp", "$root\src\speedmonitor.cpp",
    (Invoke-Moc "$root\src\speedmonitor.h")) -ExtraLibs $netLibs

# Toggle visual-state guard (offscreen; needs QtWidgets, not just QtCore).
Write-Host "==> building test_toggle" -ForegroundColor Cyan
$toggleExe = Join-Path $out 'test_toggle.exe'
$toggleArgs = @(
    '-std=gnu++17', '-O1', '-DWIN32', '-DUNICODE', '-D_UNICODE',
    "-I$root\src", "-I$qt\include", "-I$qt\include\QtCore", "-I$qt\include\QtGui",
    "-I$qt\include\QtWidgets", "-I$qt\mkspecs\win32-g++",
    "$root\tools\test_toggle.cpp",
    "$root\src\toggleswitch.cpp",
    "$root\src\theme.cpp",
    "$root\src\checkmark.cpp",
    (Invoke-Moc "$root\src\toggleswitch.h"),
    '-o', $toggleExe, "-L$qt\lib", '-lQt6Widgets', '-lQt6Gui', '-lQt6Core', '-ldwmapi', '-lmingw32'
)
& $gpp @toggleArgs
if ($LASTEXITCODE -ne 0) { throw 'test_toggle failed to compile' }
Write-Host "==> running test_toggle" -ForegroundColor Cyan
$env:QT_QPA_PLATFORM = 'offscreen'
Push-Location $out
try { & $toggleExe; $rc = $LASTEXITCODE } finally { Pop-Location }
if ($rc -ne 0) { throw 'test_toggle reported failures' }

Write-Host "`nAll tests passed." -ForegroundColor Green
