# Builds and runs the dark/light theme suite.
#
# Renders the real widgets in both themes and compares pixels.
# platform, so no display is needed. Each check is written to fail against the
# buggy behaviour it guards, not merely to restate the implementation.
#
# Needs no Administrator rights and never touches the network.
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$qt    = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'

$env:Path = "$mingw;$qt\bin;$env:Path"
$gpp = "$mingw\g++.exe"
$out = Join-Path $root 'build\tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$moc = "$qt\bin\moc.exe"
$mocOut = Join-Path $out 'moc'
New-Item -ItemType Directory -Force -Path $mocOut | Out-Null

# moc every Q_OBJECT header, since the test links the widgets directly.
$headers = Get-ChildItem "$root\src" -Filter '*.h' | Where-Object {
    (Get-Content $_.FullName -Raw) -match 'Q_OBJECT'
}
foreach ($h in $headers) {
    $target = Join-Path $mocOut ("moc_" + ($h.BaseName) + ".cpp")
    & $moc "-I$root\src" "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" `
           "-I$qt\include\QtWidgets" "-I$qt\include\QtSvg" "-I$qt\include\QtNetwork" $h.FullName -o $target
    if ($LASTEXITCODE -ne 0) { throw "moc failed for $($h.Name)" }
}
Write-Host "    moc'd $($headers.Count) headers" -ForegroundColor DarkGray

$sources = @(
    "$root\tools\test_monochrome.cpp",
    "$root\src\mainwindow.cpp",
    "$root\src\scrollbaranimator.cpp",
    "$root\src\crossticon.cpp",
    "$root\src\autostart.cpp",
    "$root\src\updater.cpp",
    "$root\src\brandmark.cpp",
    "$root\src\busydots.cpp",
    "$root\src\responsivepair.cpp",
    "$root\src\appactivitypanel.cpp",
    "$root\src\appnames.cpp",
    "$root\src\appmonitor.cpp",
    "$root\src\diagnosticspanel.cpp",
    "$root\src\diagnostics.cpp",
    "$root\src\appruledialog.cpp",
    "$root\src\applimiterpage.cpp",
    "$root\src\applimitengine.cpp",
    "$root\src\scheduledialog.cpp",
    "$root\src\schedulecard.cpp",
    "$root\src\sidebar.cpp",
    "$root\src\navicons.cpp",
    "$root\src\navbutton.cpp",
    "$root\src\theme.cpp",
    "$root\src\checkmark.cpp",
    "$root\src\speedgraph.cpp",
    "$root\src\trackslider.cpp",
    "$root\src\segmentedbar.cpp",
    "$root\src\toggleswitch.cpp",
    "$root\src\daypicker.cpp",
    "$root\src\rateeditor.cpp",
    "$root\src\rulerow.cpp",
    "$root\src\schedule.cpp",
    "$root\src\units.cpp",
    "$root\src\settingsstore.cpp",
    "$root\src\speedmonitor.cpp",
    "$root\src\usagetracker.cpp",
    "$root\src\throttleengine.cpp"
) + (Get-ChildItem $mocOut -Filter '*.cpp' | ForEach-Object { $_.FullName })

$exe = Join-Path $out 'test_monochrome.exe'
$winder = "$root\third_party\WinDivert\WinDivert-2.2.2-A\x64"

Write-Host '==> building test_monochrome' -ForegroundColor Cyan
& $gpp -std=gnu++17 -O1 -DWIN32 -DUNICODE -D_UNICODE `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    -DNTDDI_VERSION=0x06010000 -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 `
    "-I$root\src" "-I$root\third_party\WinDivert\WinDivert-2.2.2-A\include" `
    "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" "-I$qt\include\QtWidgets" "-I$qt\include\QtSvg" "-I$qt\include\QtNetwork" `
    "-I$qt\mkspecs\win32-g++" `
    @sources -o $exe "-L$qt\lib" -ladvapi32 -ldwmapi -ltdh -lwintrust -lcrypt32 -lQt6Svg -lQt6Widgets -lQt6Gui -lQt6Core -lQt6Network "-L$winder" -lwindivert `
    -liphlpapi -lws2_32 -lshell32 -lmingw32
if ($LASTEXITCODE -ne 0) { throw 'test_monochrome failed to compile' }

# Stage the Qt runtime beside the test, or it dies with STATUS_DLL_NOT_FOUND.
Write-Host '==> staging Qt runtime' -ForegroundColor Cyan
foreach ($dll in @("$qt\bin\Qt6Core.dll", "$qt\bin\Qt6Gui.dll", "$qt\bin\Qt6Widgets.dll",
                   "$mingw\libgcc_s_seh-1.dll", "$mingw\libstdc++-6.dll",
                   "$mingw\libwinpthread-1.dll")) {
    if (Test-Path $dll) { Copy-Item $dll $out -Force }
}
$platformDir = Join-Path $out 'platforms'
New-Item -ItemType Directory -Force -Path $platformDir | Out-Null
Copy-Item "$qt\plugins\platforms\qoffscreen.dll" $platformDir -Force
if (Test-Path "$qt\plugins\platforms\qwindows.dll") {
    Copy-Item "$qt\plugins\platforms\qwindows.dll" $platformDir -Force
}
Copy-Item "$winder\WinDivert.dll" $out -Force -ErrorAction SilentlyContinue

Write-Host '==> running test_monochrome (offscreen)' -ForegroundColor Cyan
$env:QT_QPA_PLATFORM = 'offscreen'
Remove-Item -Recurse -Force (Join-Path $out 'mono_scratch') -ErrorAction SilentlyContinue

Push-Location $out
try {
    & $exe
    $rc = $LASTEXITCODE
}
finally {
    Pop-Location
}

$results = Join-Path $out 'mono_results.txt'
if (Test-Path $results) { Get-Content $results | ForEach-Object { Write-Host $_ } }

if ($rc -ne 0) { throw "test_monochrome reported failures (exit $rc)" }
Write-Host "`ntest_monochrome passed." -ForegroundColor Green
