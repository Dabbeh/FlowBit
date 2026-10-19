# Plan: make the driver ready without the user having to do anything

## The problem, stated exactly

On launch, Flowbit checks whether the driver can be used. If a previous instance is still
shutting down, it cannot, and the check reports `Held`. Flowbit then shows a message saying limits
will apply when it exits — **and does nothing further**. The limits only apply at the next settings
change or the next launch.

So there are two faults, not one:

1. **The state is noticed once and never rechecked.** It is transient by nature.
2. **It is reported as an alert.** A two-second wait does not warrant a red card that the user
   has to read and dismiss.

## What "done" looks like

- Launch Flowbit twice in quick succession.
- The second instance waits, quietly, in the status bar.
- Within a second or two the limits are applied **without the user touching anything**.
- The message clears itself.
- If the driver never becomes free, the wait is abandoned after a bounded period and the message
  becomes a persistent one that says so.

## The design

### Where the poll goes

`MainWindow::persistAndApply()` is where the readiness check already lives, guarded by a
`static bool driverChecked` so it runs once. The guard is what must change: the check has to be
able to run again, and the retry has to be able to reach it.

A `QTimer` member, single-shot, restarted while the state is `Held`. On each tick the engine's
readiness is rechecked. When it stops being `Held`, the limits are applied and the timer stops.

### Why a timer and not a thread

The check is a registry read and, at most, an `OpenService` — microseconds. Doing it on the event
loop costs nothing and avoids a second thread whose only job is to wait. The engine's own worker
threads are for packet work, which is a different thing.

### Why bounded

If another process has crashed while holding the driver, or a security product is blocking it, the
state does not resolve. An unbounded poll would be a background loop for the life of the session,
doing nothing. After the window expires the message becomes persistent, which is the honest
description.

`kDriverRetryLimit` attempts at `kDriverRetryMs` apart — a few seconds in total, which is far
longer than a process teardown needs and short enough that a genuinely stuck state is reported
rather than hidden.

### The message

| State | Where it is shown | Why |
|---|---|---|
| `Held`, within the retry window | **Status bar** | A wait that resolves itself; nothing to act on |
| `Held`, after the retry window | **Error strip** | It has not resolved, so it is now a fault |
| `NeedsRestart` | Error strip | Only a restart clears it — the user must act |
| `Repaired`, `Ready` | Nowhere | Nothing needed doing |

### Applying the limits afterwards

The retry must re-run the whole apply, not just the driver check — the limits are what the user
actually wants. `persistAndApply()` already does the right thing end to end, so the retry calls it
again rather than duplicating any of it.

## What must not change

- **The driver registration logic.** Adding a missing service on launch is correct and verified;
  the retry is about wait states, not registration.
- **The single-attempt repair of a disabled service.** It is deliberate: one repair, one retry,
  then report.
- **The worker's own error paths.** A limit that fails to apply must still say so.

## Verification

- A test that forces the `Held` state and confirms the retry runs, applies the limits and stops.
- A test that the retry gives up after the bound rather than polling forever.
- A test that the status bar carries the transient message and the strip does not.
- The existing suites, unchanged, to confirm nothing else moved.

## Order

1. The retry timer and its state on `MainWindow`.
2. Route `Held` to the status bar, and to the strip only after the bound.
3. Apply the limits when the state clears.
4. The tests above.
5. Rebuild the installer.
