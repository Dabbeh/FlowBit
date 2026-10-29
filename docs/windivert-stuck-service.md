# The stuck WinDivert service: what it is, and how it is cleared without a reboot

> Correction (2026-10-23): WinDivert deliberately marks its temporary service for
> deletion after loading. A usable driver in this state needs no repair. See
> [the service lifecycle correction](windivert-service-lifecycle.md).

## The state

Found on a real machine, after an update:

```
DeleteFlag : 1        the service is marked for deletion
Start      : 4        DISABLED
State      : RUNNING  the driver is loaded anyway
```

Each line is unremarkable. Together they are a deadlock:

- A **disabled** service can never be started again.
- A **loaded** driver cannot be deleted — the pending deletion completes only when the loaded
  instance unloads.
- So the service can never start, and can never be deleted.

Flowbit reported it, correctly, as *"pending removal, which only a restart of Windows clears"* —
and that was true of the code as written, which returned `NeedsRestart` and stopped.

## Why it happens, and why it is not rare

The sequence is an ordinary update:

1. Flowbit is running, so the driver is loaded and a handle is open on it.
2. Flowbit is uninstalled, or updated in place.
3. The uninstaller runs `sc stop` then `sc delete`. The stop returns as soon as the stop is
   **requested**, not when the driver has gone; the delete then marks the service and sets
   `Start = 4` while the driver is still loaded.
4. The window in which that happens is small but entirely predictable, because the driver is
   almost always loaded when an uninstall begins — Flowbit was just running.

So a user who uninstalls and reinstalls, or updates, can land in a state where the application
reports the same fault on every launch and nothing in the application can clear it. Telling them
to restart their computer is not an acceptable answer, and worse, it is not a necessary one.

## What the fix has to do, and the constraint that shapes it

**It has to run on every launch, for every user, with no action from them.**

That constraint rules out more than it first appears:

| Route | Why it is not enough |
|---|---|
| Repair inside the packet worker | The worker starts only when a limit is set. A fresh install has limiting off, so the repair never runs — this was the first attempt, and it fixed nothing. |
| A one-off installer step | It runs at install time. A user who updates in place never triggers it. |
| Telling the user to run a command | Not a fix. This is the thing being eliminated. |
| Asking for elevation | Flowbit is already installed elevated and runs elevated. No new prompt is needed, and none may be introduced. |

So the repair belongs in `ensureDriverReady()`, which runs on **every launch** regardless of
whether limiting is on — and it must not depend on the user doing anything.

## The clearing procedure

The deadlock is broken by attacking the `Start = 4`, because that is what prevents the driver from
ever unloading cleanly.

1. **Set the start type back to demand.** A disabled service is not controllable; this is what
   makes the next step possible at all.
2. **Stop the service.** This asks the loaded driver to unload. The pending deletion then completes
   on its own — the deletion was never waiting for a reboot, it was waiting for the unload.
3. **Verify by re-reading the state**, rather than assuming the stop worked.

### When the stop is refused

Windows may refuse, and the reason matters:

- **Something else holds the driver** — another copy of Flowbit, or another application using its
  own WinDivert. The stop fails with `ERROR_SERVICE_CANNOT_ACCEPT_CTRL` or similar.
- **The service is already gone** — the deletion completed, which is success.

Both are handled rather than treated as failure. If the driver is held elsewhere, the service is
left **startable** rather than disabled, which is a strictly better state: the driver can be used,
and the deletion completes whenever that holder lets go.

**The start-type change is deliberately not rolled back** when the stop fails. A startable service
that is still marked for deletion will finish deleting itself at the next opportunity; a disabled
one never will.

## The second half: it must not happen again

Clearing the state helps machines that already have it. Stopping it recurring matters more.

The uninstaller now runs a script that:

1. stops the service,
2. **polls until it reports `STOPPED`**, up to a deadline,
3. **only then** deletes it,
4. and **skips the deletion entirely** if it never stops.

Leaving a stopped service registered is recoverable — the next install repairs it. Leaving one
marked for deletion while its driver is loaded is not, without a restart. The script prefers the
recoverable mistake to the unrecoverable one.

## What was measured, and what is assumed

**Measured:**

- The state itself: `DeleteFlag = 1`, `Start = 4`, `State = RUNNING`, read from the registry and
  the SCM on the affected machine.
- That `WinDivertOpen` creates the service — a probe opened the driver with a filter matching
  nothing and the service appeared.
- That opening the driver with a filter matching nothing registers the service **without**
  intercepting traffic.

**Assumed, and therefore checked at runtime rather than trusted:**

- That `ChangeServiceConfig` is permitted on a service whose deletion is pending. Windows may
  refuse with `ERROR_SERVICE_MARKED_FOR_DELETE`. The code does not depend on it succeeding: if the
  change is refused, the stop is still attempted, and if both are refused the state is reported
  as it was found.
- That `ControlService(SERVICE_CONTROL_STOP)` is accepted. The service's `dwControlsAccepted` is
  not guaranteed to include `SERVICE_ACCEPT_STOP`. The call's result is checked and both outcomes
  are handled.

Neither assumption is load-bearing. Each step is attempted independently, the outcome is read
back from the system, and the reported state is whatever is actually true afterwards — never what
the code hoped it had achieved.
