# Stop the driver service before any file is copied. Runs from PrepareToInstall, which Inno
# calls before the [Files] section - so the driver is unloaded while the old .sys is still in
# place, and the new one is written to a file nothing holds open.
#
# WHY THIS EXISTS
#
# An update replaces WinDivert64.sys. Windows does not permit the file of a LOADED driver to be
# replaced, and when setup forces the issue it reacts by disabling the service - leaving
#
#     Start      : 4        the service can never load again
#     State      : RUNNING  but the driver is still in kernel memory
#     DeleteFlag : 1        and a deletion may be pending on it
#
# A service that cannot load and a driver that cannot be unloaded is a deadlock. The application
# then reports it on every launch and the only way out is a restart. Closing Flowbit first is not
# enough: that releases the HANDLE, and the driver stays loaded until the service is stopped.
#
# WHAT IT DOES
#
#   1. Stop the service.
#   2. Poll until it reports STOPPED, up to a deadline.
#   3. Report whether it worked, so setup can decide.
#
# It does NOT delete the service. On an update the service is wanted - it is about to be used with
# the new driver - and only the loaded instance needs to go.
#
# Exits 0 when the driver is no longer loaded, 1 when it could not be unloaded in time. The caller
# treats 1 as a warning rather than a failure: setup can proceed, and if the copy then damages the
# service the application repairs it at the next launch.

$ErrorActionPreference = 'Continue'

$name = 'WinDivert'

function Get-State {
    param([string]$ServiceName)
    $out = & sc.exe query $ServiceName 2>&1 | Out-String
    if ($out -match '1060|does not exist') { return 'ABSENT' }
    if ($out -match 'STATE\s+:\s+\d+\s+(\w+)') { return $Matches[1] }
    return 'UNKNOWN'
}

$state = Get-State $name

if ($state -eq 'ABSENT') {
    # No service at all. Nothing is loaded, so nothing can be damaged by the copy - and setup
    # will register it afterwards.
    exit 0
}

if ($state -eq 'STOPPED') {
    exit 0
}

# A disabled service is not controllable, so the start type is set to demand first. This is the
# same step the application's own repair performs, and it is what makes the stop possible.
& sc.exe config $name start= demand | Out-Null

& sc.exe stop $name | Out-Null

# Poll rather than sleep a fixed amount. `sc stop` returns when the stop is REQUESTED, not when
# the driver has gone, and the delay varies with what else is using it.
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $state = Get-State $name
    if ($state -eq 'STOPPED' -or $state -eq 'ABSENT') { break }
}

if ($state -eq 'STOPPED' -or $state -eq 'ABSENT') {
    exit 0
}

# Still loaded. Something else holds it - another copy of Flowbit, or another application using
# WinDivert. Setup is told so it can warn, but the copy proceeds: refusing to install because a
# driver could not be unloaded would be worse than installing and repairing afterwards.
exit 1
