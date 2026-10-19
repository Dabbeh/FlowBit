# The WinDivert service: why it reads DISABLED, and why that is not a fault

## What the machine shows

```
Start       : 4        DISABLED
DeleteFlag  : 1        a deletion was requested and never completed
State       : Running  the driver is loaded and shaping traffic
CIM StartMode: Disabled   ...and Windows agrees it is loaded anyway
```

Three separate things are tangled here, and only one of them is a real problem.

## Research findings

### 1. Start = 4 is normal for WinDivert, not a fault

Measured, not assumed: the Windows management interface reports

```
State     : Running
StartMode : Disabled
```

A service configured as *disabled* cannot be started by the Service Control Manager - `StartService`
returns `ERROR_SERVICE_DISABLED` (1058). Yet this driver is loaded. The only way both can be true is
that WinDivert loads its driver by another route, one that does not consult the service's start
type. WinDivert ships an installer of its own and manages the driver's lifetime itself; the service
key is the vehicle, not the policy.

Flowbit's own code never writes a disabled start type - verified by sweeping `src/`, `installer/`
and the two installer scripts. The value is what WinDivert registered.

**Consequence: the diagnostics page has been reporting a normal state as a warning**, with text
claiming "after a restart Windows will refuse to load it until the start type is restored". That
restart happened, the driver loaded, and limits worked. The message was wrong.

### 2. The "repair" was making things worse

`repairDisabledDriverService()` reacts to a disabled start type by setting the start type to demand
and then **stopping the service**. On a machine where the driver is loaded and working, that is
actively harmful:

- stopping the driver interrupts shaping for every application, mid-session;
- a pending deletion completes as soon as the last handle closes - so stopping is exactly what lets
  a `DeleteFlag` take effect at an arbitrary moment;
- WinDivert simply reloads it on the next open, which produces another "service was installed"
  event and, on some sequences, another pending delete.

The event log shows the service being installed repeatedly - 22:26:36, 22:26:40, 22:27:03,
00:08:54, 03:03:17, 03:03:20 - which is consistent with open/stop/reload cycling rather than with a
single stable registration.

### 3. DeleteFlag = 1 is the one genuine problem, and we created it

`DeleteFlag` is set by `DeleteService` - that is, by `sc delete`. Flowbit's own uninstaller ran

```
sc stop WinDivert
sc delete WinDivert
```

The stop returns when the stop is *requested*; the delete then marks the service while the driver is
still loaded. Windows completes such a deletion at the next boot **only if the driver is not loaded
at shutdown** - and Flowbit keeps running in the tray, holding the driver, so at shutdown it is
still loaded. The deletion never completes, `DeleteFlag` stays set forever, and the state survives
every restart.

So the flag is a fossil of our own uninstall, not something Windows or WinDivert did.

## What this means for the fix

| Item | Decision |
|---|---|
| Start = 4 | **Leave it alone.** Stop "repairing" it. Report it as information, not a fault |
| Stopping the driver to repair | **Remove.** Never stop a working driver |
| `sc delete` at uninstall | **Remove.** WinDivert owns its service; deleting it is what creates `DeleteFlag` |
| Replacing the driver files on every install | **Skip when identical.** Copying over a loaded `WinDivert64.sys` is what invites Windows' driver-replacement disable in the first place |
| A machine already carrying `DeleteFlag` | Cannot be cleared without a reboot and a clean unload: the key must not be recreated, which is exactly what the two changes above prevent |

## The plan

1. **Diagnostics**: `Start = 4` becomes *information* when the driver is loaded, with text that
   states what is actually true - WinDivert manages its own service and loads the driver itself.
   When the driver is **not** loaded it stays a warning, with the real remedy (it will be loaded on
   the next open; if that fails, the driver files or the signature are the problem).
2. **Repair**: `repairDisabledDriverService()` no longer changes the start type or stops anything
   while the driver is loaded - it reports ready. Only a service that is disabled *and* not loaded
   gets a start-type correction, and even then nothing is stopped.
3. **Uninstaller**: stop the driver, do not delete the service. The delete is what breaks machines.
4. **Installer**: skip copying the two WinDivert files when the installed copies are identical, so
   an update never touches a loaded driver.
5. **Tests**: the diagnostics rule, the no-stop repair, the absent `sc delete`, and the skip-copy
   check - each asserted, because each of them is a rule that will otherwise be quietly undone.

## What this does not fix

A machine that already has `DeleteFlag` set keeps it until the driver is unloaded and the machine
restarts with Flowbit *not* holding it. The changes above stop new occurrences; they cannot clear an
existing one, and claiming otherwise would be the same mistake as before.
