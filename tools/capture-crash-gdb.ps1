# Captures a real stack trace for the intermittent Show Less crash.
#
# Why this approach: seven crash dumps exist and six of them fault at the exact same
# place, Qt6Core.dll +0x216851, which disassembles to
#
#     mov 0x48(%rcx),%rax   ; load a pointer out of an object member
#     test %rax,%rax        ; the pointer itself is non-null ...
#     cmpq $0x0,(%rax)      ; ... but what it points at has been FREED   <-- fault
#
# That is Qt reading a container whose owner was already destroyed. Freed heap
# pages usually stay mapped, which is why the crash is intermittent - exactly the
# "sometimes it works, sometimes it doesn't" the user described.
#
# A minidump records the fault address but not the call stack, and reading the code
# has produced several plausible-but-wrong candidates. The debug build carries full
# DWARF info (verified: gdb resolves src\appactivitypanel.cpp line by line), so
# running it under gdb yields the Flowbit frames that led to the fault.

param(
    [string]$ExeDir = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Flowbit'),
    [string]$DebugExe = 'C:\Users\enaya\Desktop\Flowbit-Source\build\debug\Flowbit.exe'
)

$gdb = 'C:\Qt\Tools\mingw1310_64\bin\gdb.exe'
$out = Join-Path $ExeDir 'crash-gdb.txt'
$batch = Join-Path $env:TEMP 'flowbit-gdb-run.txt'

if (-not (Test-Path $DebugExe)) { Write-Host "debug build missing: $DebugExe" -ForegroundColor Red; exit 1 }

# The debug build needs the Qt DLLs and the driver next to it. The runnable folder
# already has them, so run the debug exe FROM that folder.
@'
set pagination off
set confirm off
set width 0
set height 0
handle SIGTRAP nostop noprint pass
run
printf "\n\n===== FAULT =====\n"
bt 40
printf "\n===== THREADS =====\n"
thread apply all bt 15
printf "\n===== END =====\n"
quit
'@ | Set-Content $batch -Encoding ASCII

Write-Host "Starting the DEBUG build under gdb, from $ExeDir" -ForegroundColor Cyan
Write-Host "  (gdb needs elevation because the app loads the packet driver)" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  In the Flowbit window: Dashboard -> Show More -> Show Less." -ForegroundColor Yellow
Write-Host "  Keep toggling until it crashes. gdb catches it and prints the stack." -ForegroundColor Yellow
Write-Host ""

$inner = "& '$gdb' -batch -x '$batch' --args '$DebugExe' 2>&1 | Tee-Object -FilePath '$out'"

Start-Process powershell -Verb RunAs -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command',
    "Set-Location '$ExeDir'; $inner"
) -Wait

if (Test-Path $out) {
    Write-Host ""
    Write-Host "Trace: $out" -ForegroundColor Green
    Get-Content $out | Select-String -Pattern 'appactivitypanel|mainwindow|appmonitor|navbutton|sidebar|SIGSEGV|Program received|^#[0-9]' |
        Select-Object -First 60
} else {
    Write-Host "No trace produced (gdb may not have been allowed to run elevated)." -ForegroundColor Red
}

