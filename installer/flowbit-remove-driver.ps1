# Stops and deregisters the WinDivert driver service, in that order.
#
# WHY THIS IS A SCRIPT AND NOT TWO sc COMMANDS
#
# `sc stop` returns as soon as the stop has been REQUESTED, not when the driver has actually
# unloaded. `sc delete` marks a service for deletion - and an attempt to mark a service whose
# driver is still loaded does not take effect immediately: Windows defers it to the next boot,
# leaving the service in the "marked for deletion" state.
#
# A reinstall then cannot register its own service, because the name is held by one that is
# halfway gone, and the application reports the only thing it can see:
#
#     The WinDivert driver service is pending removal, which only a restart of Windows clears.
#
# That message is accurate and the situation is avoidable. It is avoidable by waiting for the
# driver to be gone before deleting the service, which is what this does.
#
# WHY IT GIVES UP RATHER THAN FORCING
#
# If the service does not stop, something is holding the driver - another copy of Flowbit running
# from elsewhere, or an application using its own WinDivert. Deleting the service then would put
# the machine into exactly the state this script exists to prevent. Leaving the service alone is
# recoverable: the next install repairs a disabled service, and an uninstall that leaves a
# service behind is far better than one that leaves a boot-time problem.

$ErrorActionPreference = 'Stop'

$name = 'WinDivert'
$deadline = (Get-Date).AddSeconds(15)

function Get-ServiceState {
    param([string]$ServiceName)
    $out = & sc.exe query $ServiceName 2>&1 | Out-String
    if ($out -match 'STATE\s+:\s+\d+\s+(\w+)') { return $Matches[1] }
    if ($out -match '1060|does not exist') { return 'ABSENT' }
    return 'UNKNOWN'
}

$state = Get-ServiceState $name

if ($state -eq 'ABSENT') {
    # Nothing to do. This is the normal case on a machine where the driver was never used.
    exit 0
}

if ($state -ne 'STOPPED') {
    & sc.exe stop $name | Out-Null

    # Poll rather than sleep a fixed amount: the unload is usually quick, and a fixed sleep is
    # either longer than it needs to be or too short to be reliable.
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $state = Get-ServiceState $name
        if ($state -eq 'STOPPED' -or $state -eq 'ABSENT') { break }
    }
}

if ($state -eq 'STOPPED') {
    # The driver is unloaded, so the deletion can complete now rather than at the next boot.
    & sc.exe delete $name | Out-Null
    exit 0
}

# Not stopped within the deadline. Deleting now would leave the service marked for removal and
# require a restart to clear - the exact fault this script exists to avoid - so nothing is done.
# The service is left registered and merely stopped, which the next install can work with.
exit 0
