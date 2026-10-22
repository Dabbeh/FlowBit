# Runs the ETW probe ELEVATED and captures its output.
#
# Rationale: the app is confirmed to be running with High integrity (elevated), yet
# StartTrace still fails. That rules out the elevation theory as the sole cause, so
# the next question is purely factual: can ANY process on this machine create an
# ETW session right now, from an elevated context?
#
# tools/etw_probe.exe answers exactly that, and unlike the app it prints raw codes:
#   ControlTrace STOP on a non-existent session  -> should be 4201 (not found)
#   StartTrace with a virgin name               -> should be 0 (success)
# If StartTrace answers 5 even when elevated, the block is environmental (security
# software / policy) and no amount of application code can work around it.

# The project this script belongs to. It used to be hard-coded to one machine's Desktop under a
# directory name ("Flowbit-Source") that no longer exists, so the probe was never found and the
# script always exited before doing anything.
$srcRoot = Split-Path -Parent $PSScriptRoot
$outDir = "$srcRoot\build\tests\etwprobe"
$probe = Join-Path $outDir 'etw_probe.exe'
$out = Join-Path $env:TEMP 'etw_probe_result.txt'

if (-not (Test-Path $probe)) { Write-Host "probe not built at $probe" -ForegroundColor Red; exit 1 }

Remove-Item $out -Force -ErrorAction SilentlyContinue

$inner = "& '$probe' *>&1 | Out-File -FilePath '$out' -Encoding UTF8"
Start-Process powershell -Verb RunAs -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $inner) -Wait

if (Test-Path $out) {
    Write-Host "=== ETW probe result (ELEVATED) ===" -ForegroundColor Cyan
    Get-Content $out
} else {
    Write-Host "no output captured" -ForegroundColor Red
}
