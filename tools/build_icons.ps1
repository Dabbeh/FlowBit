# Regenerates every Flowbit icon asset from the vector source.
#
# The SVGs in docs/ are the single source of truth. Everything else - the size
# set, the ICO and the copy that ships beside the executable - is derived from
# them, so a change to the artwork propagates without any bitmap ever being
# hand-edited or upscaled.
#
# Requires the bundled Qt (QSvgRenderer via tools/svg_render.cpp).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_icons.ps1
$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$qt    = 'C:\Qt\6.10.0\mingw_64'
$mingw = 'C:\Qt\Tools\mingw1310_64\bin'
$env:Path = "$mingw;$qt\bin;$env:Path"

$docs   = Join-Path $root 'docs'
$src    = Join-Path $root 'src'
$work   = Join-Path $root 'build\icons'
New-Item -ItemType Directory -Force -Path $work | Out-Null

# The sizes Windows actually asks for: tray (16/24), taskbar (32/48),
# Explorer (64/128/256) and store/detail views (512).
$sizes = @(16, 24, 32, 48, 64, 128, 256, 512)

# ---------------------------------------------------------------------------
# 1. Build the rasterizer if needed
# ---------------------------------------------------------------------------
$renderer = Join-Path $work 'svg_render.exe'
Write-Host '==> building svg_render' -ForegroundColor Cyan
& "$mingw\g++.exe" -std=gnu++17 -O1 -DWIN32 -DUNICODE -D_UNICODE `
    -finput-charset=UTF-8 -fexec-charset=UTF-8 `
    "-I$qt\include" "-I$qt\include\QtCore" "-I$qt\include\QtGui" "-I$qt\include\QtSvg" `
    "-I$qt\mkspecs\win32-g++" `
    (Join-Path $root 'tools\svg_render.cpp') -o $renderer `
    "-L$qt\lib" -lQt6Svg -lQt6Gui -lQt6Core -lmingw32
if ($LASTEXITCODE -ne 0) { throw 'svg_render failed to compile' }

# Qt runtime beside it so it can run headless.
foreach ($d in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Svg.dll')) {
    Copy-Item "$qt\bin\$d" $work -Force -ErrorAction SilentlyContinue
}
foreach ($d in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item "$mingw\$d" $work -Force -ErrorAction SilentlyContinue
}
$plat = Join-Path $work 'platforms'
New-Item -ItemType Directory -Force -Path $plat | Out-Null
Copy-Item "$qt\plugins\platforms\qoffscreen.dll" $plat -Force -ErrorAction SilentlyContinue

$env:QT_QPA_PLATFORM = 'offscreen'

# ---------------------------------------------------------------------------
# 2. Rasterize both variants at every size
# ---------------------------------------------------------------------------
function Render-Variant {
    param([string]$Svg, [string]$Prefix, [int[]]$Sizes)

    Write-Host "==> rendering $([System.IO.Path]::GetFileName($Svg))" -ForegroundColor Cyan
    Push-Location $work
    try {
        & $renderer $Svg $Prefix @($Sizes | ForEach-Object { "$_" })
        if ($LASTEXITCODE -ne 0) { throw "rasterising $Svg failed" }
    }
    finally {
        Pop-Location
    }
}

Render-Variant -Svg (Join-Path $docs 'flowbit-dark.svg')  -Prefix 'dark'  -Sizes $sizes
Render-Variant -Svg (Join-Path $docs 'flowbit-light.svg') -Prefix 'light' -Sizes $sizes

# ---------------------------------------------------------------------------
# 3. Assemble the multi-size ICO files
#
# Pillow writes a real multi-image ICO (every size is its own PNG payload), which
# is what lets Windows pick a native rendering per context instead of scaling one
# bitmap down.
# ---------------------------------------------------------------------------
Write-Host '==> assembling ICO files' -ForegroundColor Cyan

$python = 'C:\Users\enaya\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
if (-not (Test-Path $python)) { $python = 'python' }

$icoScript = Join-Path $work 'make_ico.py'
@"
import os, sys
from PIL import Image

work = r'$work'
src  = r'$src'
sizes = [int(s) for s in '$($sizes -join ',')'.split(',')]

for variant, out in (('dark', os.path.join(src, 'flowbit.ico')),
                     ('light', os.path.join(src, 'flowbit-light.ico'))):
    frames = []
    for s in sizes:
        p = os.path.join(work, f'{variant}-{s}.png')
        if os.path.exists(p):
            frames.append(Image.open(p).convert('RGBA'))
    if not frames:
        print(f'  no frames for {variant}'); continue
    # Pillow needs the largest image plus the size list to emit every entry.
    frames[-1].save(out, format='ICO', sizes=[(f.width, f.height) for f in frames])
    print(f'  wrote {out} ({os.path.getsize(out)} bytes, {len(frames)} sizes)')
"@ | Set-Content -Path $icoScript -Encoding UTF8
& $python $icoScript
if ($LASTEXITCODE -ne 0) { throw 'ICO assembly failed' }

# ---------------------------------------------------------------------------
# 4. Publish the size set into docs/ for reference and packaging
# ---------------------------------------------------------------------------
Write-Host '==> publishing PNG size set' -ForegroundColor Cyan
foreach ($s in $sizes) {
    Copy-Item (Join-Path $work "dark-$s.png")  (Join-Path $docs "flowbit-dark-$s.png")  -Force
    Copy-Item (Join-Path $work "light-$s.png") (Join-Path $docs "flowbit-light-$s.png") -Force
}
# Canonical names for the primary (dark) variant.
Copy-Item (Join-Path $work 'dark-512.png') (Join-Path $docs 'flowbit-512.png') -Force
Copy-Item (Join-Path $work 'dark-256.png') (Join-Path $docs 'flowbit-256.png') -Force
Copy-Item (Join-Path $work 'dark-128.png') (Join-Path $docs 'flowbit-128.png') -Force
Copy-Item (Join-Path $work 'dark-64.png')  (Join-Path $docs 'flowbit-64.png')  -Force
Copy-Item (Join-Path $work 'dark-32.png')  (Join-Path $docs 'flowbit-32.png')  -Force
Copy-Item (Join-Path $work 'dark-16.png')  (Join-Path $docs 'flowbit-16.png')  -Force

Write-Host ''
Write-Host 'Icon assets regenerated from SVG:' -ForegroundColor Green
Get-ChildItem $src -Filter 'flowbit*.ico' | ForEach-Object {
    Write-Host ("  {0}  ({1:N0} bytes)" -f $_.Name, $_.Length)
}
Get-ChildItem $docs -Filter 'flowbit-*.png' | Sort-Object Name | ForEach-Object {
    Write-Host ("  docs/{0}  ({1:N0} bytes)" -f $_.Name, $_.Length)
}
