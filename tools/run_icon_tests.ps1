# Verifies the application draws the real Flowbit logo.
#
# Renders the production brand mark and compares it against the reference
# artwork, then checks the silhouette is two flowing ribbons rather than a
# conventional letter F.
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$qt    = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'
$env:Path = "$mingw;$qt\bin;$env:Path"

$out = Join-Path $root 'build\tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# The test reads the reference PNG relative to its working directory.
Copy-Item (Join-Path $root 'docs\flowbit-dark-256.png') $out -Force -ErrorAction SilentlyContinue

Write-Host '==> moc-ing the widgets the test links' -ForegroundColor Cyan
# BusyDots declares Q_OBJECT, so its meta-object has to be generated or the link fails with
# "undefined reference to net::BusyDots::staticMetaObject" and "vtable for net::BusyDots".
#
# brandmark.h and theme.h have no Q_OBJECT, so moc-ing them produces nothing and is skipped
# by the size check below. Running moc over the three uniformly is simpler than tracking
# which need it, and moc writes an empty file rather than failing when there is nothing to do.
$moc = Join-Path $qt 'bin\moc.exe'
$mocOut = Join-Path $out 'moc'
New-Item -ItemType Directory -Force -Path $mocOut | Out-Null

$mocSources = @()
foreach ($h in @('busydots.h', 'brandmark.h', 'theme.h')) {
    $header = Join-Path $root "src\$h"
    if (-not (Test-Path $header)) { continue }
    $target = Join-Path $mocOut ('moc_' + [System.IO.Path]::GetFileNameWithoutExtension($h) + '.cpp')
    & $moc "-I$root\src" "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" `
        "-I$qt\include\QtWidgets" "-I$qt\mkspecs\win32-g++" `
        $header -o $target
    if ($LASTEXITCODE -ne 0) { throw "moc failed for $h" }
    # Only non-empty output is a real meta-object; an empty file means the header declared
    # no Q_OBJECT and compiling it would add nothing.
    if ((Get-Item $target).Length -gt 0) { $mocSources += $target }
}
Write-Host "    moc'd $($mocSources.Count) header(s)" -ForegroundColor DarkGray

Write-Host '==> building test_icon' -ForegroundColor Cyan
$exe = Join-Path $out 'test_icon.exe'
& "$mingw\g++.exe" -std=gnu++17 -O1 -DWIN32 -DUNICODE -D_UNICODE `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    "-I$root\src" "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" `
    "-I$qt\include\QtWidgets" "-I$qt\include\QtSvg" "-I$qt\mkspecs\win32-g++" `
    (Join-Path $root 'tools\test_icon.cpp') `
    (Join-Path $root 'src\brandmark.cpp') `
    (Join-Path $root 'src\busydots.cpp') `
    (Join-Path $root 'src\theme.cpp') `
    (Join-Path $root 'src\checkmark.cpp') `
    @mocSources `
    -o $exe "-L$qt\lib" -lQt6Svg -lQt6Widgets -lQt6Gui -lQt6Core -ldwmapi -lmingw32
if ($LASTEXITCODE -ne 0) { throw 'test_icon failed to compile' }

Write-Host '==> staging Qt runtime' -ForegroundColor Cyan
foreach ($d in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Svg.dll')) {
    Copy-Item "$qt\bin\$d" $out -Force -ErrorAction SilentlyContinue
}
foreach ($d in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item "$mingw\$d" $out -Force -ErrorAction SilentlyContinue
}
$plat = Join-Path $out 'platforms'
New-Item -ItemType Directory -Force -Path $plat | Out-Null
Copy-Item "$qt\plugins\platforms\qoffscreen.dll" $plat -Force -ErrorAction SilentlyContinue

Write-Host '==> running test_icon (offscreen)' -ForegroundColor Cyan
$env:QT_QPA_PLATFORM = 'offscreen'
Push-Location $out
try {
    & $exe
    $rc = $LASTEXITCODE
}
finally {
    Pop-Location
}

$results = Join-Path $out 'icon_results.txt'
if (Test-Path $results) { Get-Content $results | ForEach-Object { Write-Host $_ } }
if ($rc -ne 0) { throw "test_icon reported failures (exit $rc)" }
Write-Host "`ntest_icon passed." -ForegroundColor Green
