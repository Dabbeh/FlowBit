# Builds Flowbit (qmake + MinGW), deploys the Qt runtime, and reports the result.
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$qt    = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'

$env:Path = "$mingw;$qt\bin;$env:Path"
if (-not (Get-Command qmake -ErrorAction SilentlyContinue)) {
    throw "qmake not found. Expected Qt at $qt"
}

$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

Push-Location $build
try {
    Write-Host '==> qmake' -ForegroundColor Cyan
    & qmake "$root\Flowbit.pro" CONFIG+=release
    if ($LASTEXITCODE -ne 0) { throw 'qmake failed' }

    Write-Host '==> mingw32-make' -ForegroundColor Cyan
    # Compiler warnings arrive on stderr, and with $ErrorActionPreference='Stop'
    # a native command writing to stderr can abort the script even when the build
    # SUCCEEDED. Determine success from the exit code and the produced binary
    # rather than from the presence of stderr text.
    #
    # The preference has to be relaxed around the call for that to hold: merely merging the
    # stream with 2>&1 is not enough, because PowerShell 7 turns a native program's stderr into
    # ErrorRecords that terminate under 'Stop'. That is exactly what happened here - the build
    # was succeeding and the script still aborted on the first compiler warning.
    $makeOutput = ''
    $makeExit = 0
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $makeOutput = & mingw32-make -j8 2>&1 | Out-String
        $makeExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    $makeOutput | Where-Object { $_ -match 'warning:' } |
        Select-Object -First 20 | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
    if ($makeExit -ne 0) {
        $makeOutput -split "`n" | Where-Object { $_ -match 'error' } |
            Select-Object -First 20 | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        throw "make failed (exit $makeExit)"
    }
}
finally {
    Pop-Location
}

$release = Join-Path $build 'release'
if (-not (Test-Path "$release\Flowbit.exe")) { throw "Flowbit.exe not produced in $release" }

Write-Host '==> windeployqt' -ForegroundColor Cyan
# windeployqt warns about missing optional DirectX shader-compiler DLLs on most
# machines. That is informational, not a failure, and it goes to stderr - so the
# exit code decides, not the presence of the warning text. The preference has to
# be relaxed around the call, otherwise PowerShell turns that stderr into a
# terminating error before the exit code is ever examined.
$savedPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $deployOutput = & "$qt\bin\windeployqt.exe" --release --no-translations `
        --no-system-d3d-compiler --no-opengl-sw "$release\Flowbit.exe" 2>&1 | Out-String
    $deployExit = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $savedPreference
}
if ($deployExit -ne 0) {
    Write-Host $deployOutput -ForegroundColor Red
    throw "windeployqt failed (exit $deployExit)"
}

# The app must carry Qt's SVG module, since the brand mark is loaded from the
# logo SVG at runtime rather than baked into a raster asset.
if (-not (Test-Path "$release\Qt6Svg.dll")) {
    throw "Qt6Svg.dll missing from $release - the brand mark would fall back to the painted approximation"
}

$exe = Get-Item "$release\Flowbit.exe"
Write-Host ("`nBuilt {0} ({1:N0} bytes)" -f $exe.FullName, $exe.Length) -ForegroundColor Green
Write-Host 'Run it elevated (the manifest requests Administrator automatically).' -ForegroundColor Yellow
