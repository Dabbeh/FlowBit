# Runs Flowbit under gdb so the intermittent crash yields a real stack trace.
#
# Seven crash dumps exist. Six fault at Qt6Core.dll +0x216851, which disassembles to
#
#     mov 0x48(%rcx),%rax      ; load a pointer from an object member
#     test %rax,%rax           ; pointer is non-null...
#     cmpq $0x0,(%rax)         ; ...but the pointee is FREED memory  <-- fault
#
# That is a Qt container read after its owner was destroyed, and because freed heap
# pages often remain mapped it only faults some of the time - matching "sometimes
# it works, sometimes it crashes".
#
# A minidump gives the fault address but not the call stack, and static reading has
# produced several plausible-but-wrong candidates. This runs the release build with
# its symbol table under gdb, so when the fault happens gdb prints the Flowbit
# frames that led to it.
#
# Usage: run this, click Show More / Show Less in the app until it crashes.
# gdb prints the backtrace to the console and to crash-gdb.txt.

param(
    [string]$ExeDir = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Flowbit'),
    [int]$TimeoutSeconds = 600
)

$exe  = Join-Path $ExeDir 'Flowbit.exe'
$gdb  = 'C:\Qt\Tools\mingw1310_64\bin\gdb.exe'
$out  = Join-Path $ExeDir 'crash-gdb.txt'
$batch = Join-Path $env:TEMP 'flowbit-gdb-run.txt'

if (-not (Test-Path $exe)) { Write-Host "Flowbit.exe not found at $exe" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $gdb)) { Write-Host "gdb not found at $gdb" -ForegroundColor Red; exit 1 }

# `handle` keeps gdb from stopping on benign traps; `bt` at the fault is the payload.
@'
set pagination off
set confirm off
set width 0
set height 0
handle SIGTRAP nostop noprint pass
handle SIGSEGV stop print
run
printf "\n\n===== FAULTING FRAME =====\n"
bt 40
printf "\n===== ALL THREADS =====\n"
thread apply all bt 12
printf "\n===== END =====\n"
quit
'@ | Set-Content $batch -Encoding ASCII

Write-Host "Launching Flowbit under gdb (elevated - it needs the packet driver)..." -ForegroundColor Cyan
Write-Host ""
Write-Host "  In the app: open the Dashboard, press Show More, then Show Less." -ForegroundColor Yellow
Write-Host "  Repeat until it crashes. gdb will catch it and print the stack." -ForegroundColor Yellow
Write-Host ""

$inner = "& '$gdb' -batch -x '$batch' --args '$exe' *>&1 | Tee-Object -FilePath '$out'"

# Run elevated, and wait: the whole point is to capture the crash.
Start-Process powershell -Verb RunAs -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
    "Set-Location '$ExeDir'; $inner"
) -Wait

if (Test-Path $out) {
    Write-Host "Trace written to $out" -ForegroundColor Green
    Write-Host ""
    Get-Content $out | Select-String -Pattern 'Flowbit|appactivitypanel|mainwindow|appmonitor|SIGSEGV|Program received|#[0-9]+' |
        Select-Object -First 50
} else {
    Write-Host "No trace file was produced." -ForegroundColor Red
}
