# The WinDivert driver service, and why limits take a moment to start

> Correction (2026-10-23): WinDivert deliberately marks its temporary service for
> deletion after loading. The disabled/pending-deletion state alone does not
> establish damage. See [the service lifecycle correction](windivert-service-lifecycle.md).

## The symptom

Flowbit opens and reports that the driver is not running or that limits are not applying.
A short time later it starts working on its own, without the user doing anything.

The state on the machine at the time:

```
> sc qc WinDivert
SERVICE_NAME       : WinDivert
TYPE               : 1  KERNEL_DRIVER
START_TYPE         : 4  DISABLED          <-- Windows will refuse to start it
BINARY_PATH_NAME   : \??\C:\Users\enaya\Desktop\Flowbit\WinDivert64.sys

> sc query WinDivert
STATE              : 4  RUNNING           <-- but it is loaded in memory right now
```

**Disabled and running at the same time.** That pairing is the whole problem, and it explains
why the failure is temporary.

## What those two numbers mean

`START_TYPE` is what Windows will do *next time* it needs to load the driver. `STATE` is what
is loaded *now*. They are set independently, and a service can be:

| START_TYPE | STATE | Meaning |
|---|---|---|
| 3 DEMAND | 4 RUNNING | Normal. Starts on request, currently loaded. |
| 3 DEMAND | 1 STOPPED | Normal. Simply not in use. |
| **4 DISABLED** | **4 RUNNING** | **The broken state.** Loaded now, unstartable later. |

In the broken state the driver is already in kernel memory, so anything that opens a handle
to the existing device succeeds immediately. Nothing needs to *start* the service, so the
DISABLED flag is never consulted. That is why the app can appear to work.

The flag bites when the service has to be started from scratch: after a reboot, after the
driver is unloaded, or when the last handle closes and the service stops itself. At that
point `StartService` returns **ERROR_SERVICE_DISABLED (1058)** and every limit fails until
something puts the start type back.

## Why Windows disabled it

Windows disables a kernel service whose registered image file is replaced underneath it.

The reasoning is a security one: an attacker who can write to a path that a kernel service
loads from could otherwise have code loaded into the kernel at the next start. Disabling the
service is the safe response when the file changes without going through a driver installer.

Flowbit is affected more than most because of **where it is normally run from**. Every
in-place update on the Desktop does this:

1. `WinDivert64.sys` is on the Desktop, inside the app folder;
2. the driver is loaded, and the service records that path;
3. Flowbit.exe is replaced - by the updater, by a rebuild, by a deploy script;
4. the folder's contents change underneath the registered path;
5. Windows notices at the next load and sets `Start = 4`.

The file itself is not the problem. The same bytes installed under `Program Files` and left
alone are fine. What matters is that the path's contents stop changing.

## Why it "starts working after a while"

It usually does not, by itself. What happens is one of:

- **A leftover handle.** Another process - or a previous Flowbit that has not fully exited -
  still holds the driver open, so `WinDivertOpen` succeeds against the loaded instance. The
  app reports healthy while the start type remains DISABLED, and the problem returns at the
  next reboot.
- **The repair succeeds after the previous instance exits.** Flowbit already tries to repair
  the service (see below). If a stale Flowbit is still holding the driver when the repair is
  attempted, the repair can fail with error 1072 and succeed on a retry a few seconds later.

Either way the app is not fixing itself on a timer. It is racing another process, and the
outcome depends on which one wins.

## What Flowbit already does

`ThrottleEngine` detects this and repairs it:

- `describeOpenError()` recognises `ERROR_SERVICE_DISABLED` and explains it in terms of a
  registry value rather than a bare code;
- `repairDisabledDriverService()` puts the start type back;
- after a successful repair, `WinDivertOpen` is retried once;
- `servicePointsAtOurDriver()` checks the registered path before deciding a repair is safe,
  so a service belonging to another application's copy of WinDivert is not commandeered.

The remaining weakness is **timing**: the repair is attempted on demand, after something has
already failed. A first launch therefore shows a failure that a second attempt would not.

## The fix, in three parts

### 1. Install to a stable location

The single most effective change, and it is a packaging one rather than a code one. The
installer places the application under `Program Files`, which the user's own updates and
builds do not rewrite. The registered driver path then stops changing, so Windows stops
disabling the service.

`installer/flowbit.iss` does this. Running from the Desktop will keep producing this state no
matter what the code does.

### 2. Repair at startup, before the first open

Instead of waiting for `WinDivertOpen` to fail, check and repair during launch:

- read the service's start type at startup;
- if it is DISABLED and the registered path is ours, set it back and start the service;
- report what was found in the diagnostics page rather than only when a limit fails.

This turns a visible failure-then-recovery into a brief delay with nothing shown, which is
what "it should start automatically" means in practice.

The check must be cheap and must not block the UI. Reading one registry value and calling
`CreateService`/`ChangeServiceConfig` is microseconds; the driver load itself is what takes
time, and that happens off the UI thread already.

### 3. Do not fight a service that is still held

Error 1072 (`ERROR_SERVICE_MARKED_FOR_DELETE`) and a held device both mean a previous
instance has not finished exiting. Only a reboot clears a marked-for-delete service. The
current code reports this correctly; the improvement is to distinguish it in the message from
the ordinary disabled case, because the remedy is different:

| State | Remedy |
|---|---|
| DISABLED, not held | Repaired automatically |
| DISABLED, but another instance running | Wait for it, or quit it from the tray |
| MARKED_FOR_DELETE | Reboot - no command can clear it |

## How to check the state by hand

```
sc qc WinDivert        # START_TYPE: 4 means disabled
sc query WinDivert     # STATE: 4 means loaded
```

In an elevated PowerShell, the repair is:

```
sc config WinDivert start= demand
sc start WinDivert
```

Note the space after `start=` - `sc` requires it.

## What was observed on this machine

```
START_TYPE : 4  DISABLED
STATE      : 4  RUNNING
BINARY_PATH: \??\C:\Users\enaya\Desktop\Flowbit\WinDivert64.sys
Registry Start value: 4
```

The registered path is correct and the file exists, so this is not a stale-path problem. It
is the disabled-but-loaded state, produced by the application folder being rewritten in place
while the driver was registered from it.
