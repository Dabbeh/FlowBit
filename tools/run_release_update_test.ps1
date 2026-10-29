param([Parameter(Mandatory=$true)][string]$FeedUrl, [string]$RunningVersion="1.0.1", [Parameter(Mandatory=$true)][string]$ExpectedSha256, [string]$ExpectedVersion="1.0.2")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$qt = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'
$env:PATH = "$mingw;$qt\bin;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_PLUGIN_PATH = "$qt\plugins"
$out = Join-Path $root 'build\tests\release-update'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# Link the real application objects, excluding the production entry point and
# elevation manifest. The test uses the actual App Limiter page and engine.
& 'C:\Qt\Tools\CMake_64\bin\cmake.exe' --build (Join-Path $root 'build-cmake') --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Application build failed' }
$objects = @(Get-ChildItem (Join-Path $root 'build-cmake\CMakeFiles\Flowbit.dir') -Recurse -Filter '*.obj' |
    Where-Object { $_.Name -notin @('main.cpp.obj', 'app.rc.obj') } |
    ForEach-Object { $_.FullName })
$exe = Join-Path $out 'test_release_update.exe'
& "$mingw\g++.exe" -std=c++17 -O1 -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB `
    '-finput-charset=UTF-8' '-fexec-charset=UTF-8' `
    "-I$root\src" "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" `
    "-I$qt\include\QtWidgets" "-I$qt\include\QtNetwork" "$root\tools\test_release_update.cpp" @objects `
    "-L$qt\lib" "-L$root\third_party\WinDivert\WinDivert-2.2.2-A\x64" `
    -lQt6Test -lQt6Svg -lQt6Network -lQt6Widgets -lQt6Gui -lQt6Core -lwindivert `
    -liphlpapi -lws2_32 -ladvapi32 -ltdh -lwintrust -lcrypt32 -ldwmapi -lpsapi `
    -lshell32 -luser32 -lwinmm -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Release update test failed to compile' }
Copy-Item "$root\third_party\WinDivert\WinDivert-2.2.2-A\x64\WinDivert.dll" $out -Force
& $exe $FeedUrl $RunningVersion $ExpectedSha256 $ExpectedVersion
exit $LASTEXITCODE
