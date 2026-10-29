# The WinDivert driver: when it is installed, and why it is not ready immediately

> Correction (2026-10-23): the service is temporary and recreated on demand. Its
> absence or pending deletion alone does not establish an installation problem.
> See [the service lifecycle correction](windivert-service-lifecycle.md).

## What the driver is

WinDivert is the kernel component that lets Flowbit hold a packet back and release it at a
controlled rate. It has two parts:

| Part | What it is |
|---|---|
| `WinDivert.dll` | The user-mode library Flowbit links against |
| `WinDivert64.sys` | The kernel driver, loaded and unloaded by Windows |
| The **service** | The registry entry that tells Windows about the driver and where it lives |

All three are needed. A missing service means the driver cannot be loaded at all, and `WinDivertOpen`
fails.

## When the service gets created

**The service is created by `WinDivertOpen`, not by installing the files.** This was measured
rather than assumed: a probe that opened the driver with a filter matching nothing and closed it
again printed

```
before: service absent
OPENED the driver
after : service EXISTS - WinDivert created it
```

The library registers the service the first time the driver is opened, using the path of the
`.sys` file found beside the running executable. That is why the driver files must sit next to the
application, and why copying them elsewhere does not help.

This is confirmed by the API itself, which offers a flag to suppress it:

```c
#define WINDIVERT_FLAG_NO_INSTALL       0x0010
```

A flag to skip installation only makes sense if opening installs.

## The consequence for Flowbit

Flowbit opens the driver only when a limit is set to something other than unlimited:

```cpp
const bool wanted = dir.rateBps.load() > 0;
if (wanted && !dir.running())
    startWorker(dir);
```

A fresh installation has limiting off. So nothing opens the driver, so the service is never
created, so the first limit a user sets fails — until they happen to run Flowbit elevated from the
folder holding both driver files, which is not something a user should have to know.

The repair path made this worse by declining to act:

```cpp
if (!state.exists)
    return DriverReadiness::Unavailable;
```

That is an accurate description and a useless repair. Both halves waited for something the other
was supposed to do.

**Fixed** by registering during installation, which is the one moment the elevation the driver
needs is already held, and by creating a missing service on launch rather than returning
`Unavailable`.

## Why the message appears briefly at launch, and goes

On a normal launch Flowbit checks the driver before applying limits. If the previous instance has
not finished shutting down, it still holds the driver, and the check reports `Held`.

Releasing the driver is not instantaneous. `WinDivertClose` asks the driver to stop; the kernel
then unloads it once the last handle is gone and any queued packets have been dealt with. That
takes a moment — long enough for a startup check to see it still held.

**So the message is accurate.** It is a wait, not a fault. It clears itself when the driver
becomes free, which is why it disappears after a few seconds.

## What was still wrong

The message was honest but the behaviour was not finished. Flowbit said "limits will apply as soon
as it exits" and then **did nothing**. The limits were only applied at the next settings change, or
at the next launch. A user who set a limit, closed the window and reopened it quickly would be told
to wait, and then see the limit not applied with nothing further to explain it.

Three things are needed, and none of them is complicated:

1. **Notice when the driver becomes free.** The state is transient and resolves itself, so a
   bounded poll is the right shape — not a single check at startup.
2. **Apply the limits then**, rather than waiting for unrelated user action.
3. **Say so quietly.** A wait that resolves itself in two seconds does not need a red card. The
   status bar is for this.

The poll must be **bounded**. If something genuinely holds the driver — another instance that has
crashed without releasing it, or a security product — the state does not resolve and a poll that
runs forever would be a background loop for the life of the session. After a reasonable window the
message becomes a persistent one, which is the honest description of a state that is not clearing.

## Why this is not fixed by retrying the open in the worker

The obvious alternative is to have the worker retry `WinDivertOpen` until it succeeds. That is
wrong here for a specific reason: the worker is only started when a limit is set, and the `Held`
state is detected at startup, before any limit exists. A worker retry would fix the case where a
limit is set while the driver is busy, and miss the case that is actually reported.

The poll belongs where the readiness check is, because that is where the state is known.

## Sources

- WinDivert API header, `basil00/WinDivert`, `include/windivert.h` — retrieved and read, and the
  source of the `WINDIVERT_FLAG_NO_INSTALL` confirmation above.
- The measured probe output in this repository's history, and the diagnostics report showing
  `[XX] WinDivert service: not registered` on a machine with both driver files present.
