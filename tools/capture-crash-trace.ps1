# Gets a real stack trace for the Show Less crash.
#
# Four candidate causes have been disproved with isolated reproducers: the
# deferred monitor stop, missing animation endpoints, re-parenting destroying the
# graphics effect, and double-deletion of animations. Guessing has cost several
# rounds, so this obtains the actual call stack.
#
# What it does:
#   1. Enables Windows local crash dumps for Flowbit.exe.
#   2. Launches Flowbit so you can reproduce the crash by clicking Show Less.
#   3. Reads the resulting dump with gdb and prints the backtrace.
#
# Run it, then click Show More and Show Less in the Flowbit window.

param([string]$ExeDir = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Flowbit'))

$exe = Join-Path $ExeDir 'Flowbit.exe'
if (-not (Test-Path $exe)) { Write-Host "Flowbit.exe not found in $ExeDir" -ForegroundColor Red; exit 1 }

$dumpDir = Join-Path $env:LOCALAPPDATA 'CrashDumps'
$gdb = 'C:\Qt\Tools\mingw1310_64\bin\gdb.exe'

Write-Host "Enabling local crash dumps for Flowbit.exe" -ForegroundColor Cyan
$reg = 'HKCU:\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\Flowbit.exe'
New-Item -Path $reg -Force | Out-Null
New-ItemProperty -Path $reg -Name 'DumpFolder' -Value $dumpDir -PropertyType ExpandString -Force | Out-Null
New-ItemProperty -Path $reg -Name 'DumpType'   -Value 2 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $reg -Name 'DumpCount'  -Value 5 -PropertyType DWord -Force | Out-Null
New-Item -Path $dumpDir -Force | Out-Null
Write-Host "  dumps go to $dumpDir"

Get-ChildItem $dumpDir -Filter 'Flowbit*.dmp' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Launching Flowbit..." -ForegroundColor Cyan
Start-Process -FilePath $exe -Verb RunAs

Write-Host ""
Write-Host "NOW reproduce it:" -ForegroundColor Yellow
Write-Host "  1. On the Dashboard, press Show More"
Write-Host "  2. Press Show Less"
Write-Host "  3. Repeat a few times if it does not crash immediately"
Write-Host ""
Read-Host "Press Enter here after the app has closed/crashed"

$dump = Get-ChildItem $dumpDir -Filter 'Flowbit*.dmp' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $dump) {
    Write-Host "No dump found - the app may not have crashed, or the dump was written elsewhere." -ForegroundColor Red
    Write-Host "Check: $dumpDir"
    exit 1
}

Write-Host ""
Write-Host "Reading dump: $($dump.FullName)" -ForegroundColor Cyan

$batch = Join-Path $env:TEMP 'flowbit-dump-gdb.txt'
@'
set pagination off
set confirm off
set width 0
echo \n===== THREADS =====\n
thread apply all bt 25
echo \n===== DONE =====\n
quit
'@ | Set-Content $batch -Encoding ASCII

# The dump is 64-bit and Flowbit.exe carries the symbols (built with -g), so gdb
# can resolve our own frames even though the Qt DLLs have none.
$trace = Join-Path $ExeDir 'crash-stack.txt'
& $gdb -batch -x $batch "$exe" "$dump.FullName" 2>&1 | Out-File $trace -Encoding UTF8

Write-Host "Trace written to: $trace" -ForegroundColor Green
Write-Host ""
Get-Content $trace | Select-String -Pattern 'Flowbit|appactivitypanel|mainwindow|Qt6|SIGSEGV|access|#0|#1|#2|#3|#4|#5|#6|#7|#8' |
    Select-Object -First 60
