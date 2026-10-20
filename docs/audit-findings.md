# Flowbit defect audit

Date: 2026-10-20. Method: compiler warnings across every translation unit, targeted pattern sweeps
for known defect classes, hand-reading of the packet-shaping, monitor, persistence and updater
paths, and comparison against the SDK headers and vendor documentation that ship with the project.

**This is not a proof of correctness.** It is a list of defects that were looked for and found, a
list of things that were suspected and cleared, and an honest statement of what was not examined.

---

## How each class was examined

| Class | Method |
|---|---|
| Type errors, unused or dead state | `g++ -fsyntax-only -Wall -Wextra -Wshadow` over all 80 sources |
| Unsigned wraparound | pattern sweep for `quint64 a = b - c`, then hand-reading the accounting path |
| Blocking on the UI thread | sweep for `waitForFinished`, `msleep`, `sleep_for`, `QProcess::execute`, then tracing callers to threads |
| Ignored API results | sweep for `WinDivertSend`, `WinDivertRecv`, `QFile::write`, `setPermissions` |
| ETW decoding | compared the decoder's type table against `TDH_IN_TYPE` in the Windows SDK on this machine |
| Buffer handling | hand-reading of the only two `memcpy` calls and the packet buffers |

---

## Findings

### F1 — Re-injected packets are sent without checking the result (HIGH)

`src/applimitengine.cpp:804` (flow worker) and `:887` (pacing worker):

```cpp
WinDivertSend(static_cast<HANDLE>(impl->flowHandle), buffer.data(), recvLen, nullptr, &addr);
```

The comment immediately above the first one says *"Flow events MUST be re-injected or the network
stack stalls."* — and the return value is discarded. WinDivert's documentation (§5.7
`WinDivertSend`) defines the call as the injection half of a recv/send pair: a failed send means
that packet is **gone**. For the flow worker, a lost flow event stalls connection tracking; for the
pacing worker, a lost packet is a dropped packet the user experiences as a stall in whatever
application was being shaped.

Nothing counts these, nothing retries, and nothing reports them. A shaping session that silently
drops 5% of packets looks exactly like a bad connection.

**Fix**: check the result, count failures, and treat a full injection queue the way the vendor
intends — a bounded retry with a short backoff rather than an unconditional drop. Surface the count
in diagnostics so a user reporting "it drops my connection" can be answered with data.

### F2 — Enabling start-with-Windows blocks the UI thread for up to 20 seconds (HIGH)

`src/mainwindow.cpp:3655` calls `autostart::setEnabled(...)` from the settings apply path, on the
GUI thread. That function shells out to `schtasks.exe` and calls
`p.waitForFinished(20000)` (`src/autostart.cpp:128`).

Qt's `QProcess::waitForFinished` documentation is explicit that it blocks the calling thread. On a
machine where the Task Scheduler service is slow, or where an antivirus inspects the spawned
process, the whole window freezes for the entire timeout. The user sees an unresponsive app for a
setting that has nothing to do with the network.

**Fix**: run the task registration off the GUI thread and report completion back to it, the same way
`AppMonitor::startAsync()` already does for the ETW session.

### F3 — Collapsing the activity panel can freeze the window for a second (MEDIUM)

`AppMonitor::stop()` (`src/appmonitor.cpp:1787`) spins for up to `100 × 10ms` waiting for an
in-flight async start, then tears the session down. The panel calls it from a deferred lambda on the
GUI thread (`appactivitypanel.cpp` finished handler).

The wait is bounded and the comment explains why it exists, but the thread it runs on is wrong: a
collapse animation that ends with a one-second freeze is exactly the "lag" this panel has already
been reported for twice.

**Fix**: provide `stopAsync()` that runs the same teardown on a worker thread, and use it from the
panel. The state machine does not change.

### F4 — Settings file permissions are set and the result ignored (MEDIUM)

`src/settingsstore.cpp:257`:

```cpp
QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner);
```

If this fails, the configuration keeps whatever ACL it inherited — and it holds installed paths,
per-application limits and schedule times. The function that follows it deliberately writes through
`QSaveFile` to avoid half-written files, so the intent to be careful is already there; this line is
the one place the result is dropped.

**Fix**: check the result and report failure through the existing `error` out-parameter rather than
returning success for a file that is world-readable.

### F5 — Dead state in the driver-service repair (LOW, but misleading)

`src/throttleengine.cpp:275` assigns `outcome` on three paths and never reads it: the function
returns from a read-back of the real service state instead. The compiler says so
(`-Wunused-but-set-variable`).

The behaviour is correct; the variable suggests the function decides from its own calls, which is
precisely the assumption that produced the earlier wrong "restart required" advice.

**Fix**: delete it and let the read-back be the single source of truth, as the comment above it
already claims.

### F6 — Two cosmetic warnings (LOW)

`src/theme.cpp:223` computes `dark` and never uses it. `src/mainwindow.cpp:867` declares `canvas`
inside a block that shadows an outer `canvas`.

**Fix**: remove one, rename the other. They cost nothing and keep a clean `-Wall` build meaningful.

---

## Suspected and cleared (recorded so nobody re-does the work)

### The ETW type table is correct

The decoder uses `inType == 8 || inType == 10` and its comment calls these UInt32/UInt64. The
Windows SDK header `TDH_IN_TYPE` on this machine numbers them:

```
0 NULL, 1 UNICODESTRING, 2 ANSISTRING, 3 INT8, 4 UINT8, 5 INT16, 6 UINT16,
7 INT32, 8 UINT32, 9 INT64, 10 UINT64, 11 FLOAT, ... 15 POINTER
```

So the code matches the SDK. **Rejecting this on memory would have broken a working decoder.**

### The monitor's blocking waits are on a worker thread

`appmonitor.cpp` waits up to 4s and 6s on `logman`/`schtasks`-style cleanup. Those calls are inside
`start()`, which `startAsync()` runs on a `QThread::create` thread — verified by reading the caller
chain, not assumed. They do not freeze the UI.

### The token-bucket and the EStats deltas are guarded

`appmonitor.cpp:961-962` and `speedmonitor.cpp:206-207` both compare before subtracting. The
per-application accounting bug that produced 8.6e18 bytes was in `UsageTracker::addAppSample`,
which added a cumulative total as if it were a delta — already found and fixed, and it is why the
sweep for unguarded `quint64` subtraction was run first.

---

## Not examined (stated plainly)

- **Concurrency in the shaping workers** beyond reading the lock usage: no model checking, no
  ThreadSanitizer equivalent on this toolchain.
- **The ~250 `new` expressions**: audited only by pattern (parented vs not), not expression by
  expression.
- **Compatibility across Windows builds** other than 26300: the ETW and driver findings on this
  machine may not generalise.
- **Memory safety** under malformed input: TDH decodes provider data, and the decoder trusts the
  buffer size it is given.

---

## Plan, in priority order

1. **F1** — result-checked packet injection with bounded retry and a failure counter surfaced in
   diagnostics. This is the only finding that can silently break a user's connection.
2. **F2** — async task registration; no UI thread waits on an external process.
3. **F3** — `stopAsync()` for the monitor, used by the panel.
4. **F4** — checked permissions with an error path.
5. **F5, F6** — dead state and warnings.

Each fix lands with a test that fails on the old behaviour, a build verified by exit code, and the
full suite sweep (no suite may report zero checks).
