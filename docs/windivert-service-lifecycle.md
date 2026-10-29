# WinDivert's temporary service and the diagnostics warning

## Correction, 2026-10-23

`Start=4` together with `DeleteFlag=1` does not by itself establish a broken
WinDivert installation. WinDivert 2.2.2 deliberately calls `DeleteService`
after successfully starting the driver. The registration is temporary and is
cleaned up when the driver stops or Windows reboots. `WinDivertOpen` creates it
again on demand.

Sources:

- [WinDivert 2.2.2 service lifecycle](https://github.com/basil00/WinDivert/blob/v2.2.2/dll/windivert.c#L306-L365)
- [WinDivert installation and removal](https://reqrypt.org/windivert-doc.html#installing)

Earlier notes in this directory attributed the disabled/pending-deletion state
to updates without accounting for that deliberate lifecycle. Their claims that
this state necessarily prevents the next launch or requires repair are superseded
by this correction.

## Current behavior

- `probeLoadedDriver()` opens a match-nothing, receive-only sniff handle with
  `WINDIVERT_FLAG_NO_INSTALL`, then closes it. It cannot install the driver or
  capture/block traffic.
- Diagnostics show a successful probe plus a temporary registration as healthy,
  retaining the raw Windows `DISABLED` reading with an expected-cleanup explanation.
- A failed probe retains its error code. Disabled registrations are not silently
  declared healthy when access could not be established.
- `ensureDriverReady()` returns immediately after a successful probe. Service
  recovery is reserved for an unavailable driver, and its ownership check compares
  the full driver path before changing configuration.

## Observed during this change

Windows reported `STATE=4 RUNNING`, `Start=4`, and `DeleteFlag=1`, with the driver
in `C:\Program Files\Flowbit`. A separate read-only open from the development
process returned error 5 (access denied); the process was not elevated. The release
build succeeded. Elevated driver access and reboot behavior were not revalidated
from this development process.
