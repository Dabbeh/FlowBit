# Runs the per-application usage tests.
#
# Compiles the tracker with the test that exercises it. The tracker is pulled in
# directly rather than through the GUI, so this runs headless and needs no display.

$ErrorActionPreference = 'Stop'
$srcRoot = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $srcRoot 'build\tests\appusage'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$env:Path = 'C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.10.0\mingw_64\bin;' + $env:Path
$moc = 'C:\Qt\6.10.0\mingw_64\bin\moc.exe'
$gxx = 'g++'

Push-Location $outDir
try {
    & $moc "-I$srcRoot\src" '-IC:\Qt\6.10.0\mingw_64\include' `
        "$srcRoot\src\usagetracker.h" -o moc_usagetracker.cpp 2>$null

    $args = @(
        '-std=c++17', '-O1', '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
        '-IC:\Qt\6.10.0\mingw_64\include',
        '-IC:\Qt\6.10.0\mingw_64\include\QtCore',
        "-I$srcRoot\src",
        "$srcRoot\tools\test_app_usage.cpp",
        "$srcRoot\src\usagetracker.cpp",
        "$outDir\moc_usagetracker.cpp",
        '-LC:\Qt\6.10.0\mingw_64\lib', '-lQt6Core',
        '-o', 'test_app_usage.exe'
    )
    $build = & $gxx @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        $build | Where-Object { $_ -match 'error:' } | Select-Object -First 8
        Write-Host 'failed to compile test_app_usage' -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}

# The test needs the Qt runtime beside it.
$qtBin = 'C:\Qt\6.10.0\mingw_64\bin'
foreach ($dll in @('Qt6Core.dll', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
    Copy-Item (Join-Path $qtBin $dll) $outDir -Force -ErrorAction SilentlyContinue
}

& (Join-Path $outDir 'test_app_usage.exe')
exit $LASTEXITCODE
