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


---

# Pass 2 and 3: concurrency, then stored data

## F7 — The pacer's rate was mutated from two threads without synchronisation (HIGH)

`src/throttleengine.h` declared the pacer's state as plain `qint64`:

```cpp
qint64 m_rate = 0;  qint64 m_burst;  qint64 m_tokens;  Clock::time_point m_last;
```

`setRate()` is called from the GUI thread (`applimitengine.cpp:1005`, when rules are re-evaluated)
while a packet worker sits in `waitFor()` reading and updating the same fields. That is a **data
race**, which is undefined behaviour in C++ — not merely a value that might be one tick stale.

What it could look like to a user: a limit that occasionally does not apply, a burst after changing
a rate, or (theoretically) a torn 64-bit read. It would never reproduce on demand.

**Fix**: all state is now behind a `std::mutex`, with the lock released for the sleep so a rate
change never waits on a pacing slice. `refill()` became `refillLocked()` so it cannot be called
without the lock. Verified by `test_pacer_race` — 400 concurrent rate changes while a worker paced,
2.8 million packets through it, no hang, no negative rate, and switching to unlimited releases a
waiting packet in 88ms instead of the multi-second deficit it was waiting out.

**Checked and sound**: the limiter's `QHash` containers (`tupleOwner`, `buckets`). The worker takes
the mutex, copies the `shared_ptr`, releases the lock, then uses it — so a rule change mid-packet
cannot invalidate what the worker holds.

## F8 — Rates were cast from `double` to `qint64` with no bounds (MEDIUM)

`settingsstore.cpp` read every rate as `RateBps(o.value(...).toDouble(0))`. Converting a double that
is negative, NaN, or outside `qint64`'s range is **undefined behaviour**; in practice it yields a
large negative number, which the pacer treats as "unlimited" — a limit the user set silently stops
working. A hand-edited or corrupted `settings.json` is enough.

`clampRate()` now bounds every rate read to 0..100 Gbit/s, and `clampDays()` handles the weekday
mask: 256 used to truncate to 0, i.e. "no days", which disabled a schedule with no visible cause.
It clamps to all-days instead.

## F9 — Usage history grew without limit (LOW)

One file per month, written forever and never removed. Nothing in the UI can reach data older than
the current cycle or the last 30 days. `pruneOldMonths()` keeps thirteen months and removes the
rest on load.

**Bug in my own fix, found by its test**: the first version split the filename on the *last* dash,
which produces `04.json` from `apps-2026-04.json`, so every file failed to parse and nothing was
ever pruned. The test caught it because it created a real 30-month-old file and asserted it was
gone — the assertion failed, the parse was inspected, the offset was fixed. A structural check
("the function exists") would have passed throughout.

---


---

# Pass 4: resource lifetime and shutdown ordering

## F10 — The ETW consumer's trace handle was closed under a running ProcessTrace (HIGH)

`AppMonitor::Impl::stopSession()` did this, in this order:

```cpp
CloseTrace(traceHandle);   // frees the handle
...
thread->wait(3000);        // consumer is still inside ProcessTrace
delete thread;             // ...even if the wait timed out
```

Two faults in five lines, both undefined behaviour:

1. **Use-after-close.** `ProcessTrace()` blocks on the consumer thread and holds the trace handle
   for as long as it runs; `CloseTrace()` releases that handle's control block. The old order freed
   it while the consumer was still using it. This is precisely the shape of a crash that happens
   "sometimes" when the activity panel collapses - and it matches the panel's history of being the
   one component reported as laggy and unstable.
2. **Deleting a running QThread.** The result of `wait(3000)` was discarded, so a slow stop deleted
   the thread object while its stack frame was still live.

The documented order is the reverse — stop the session (which is what makes `ProcessTrace` return),
join the consumer, and only then close the handle. The fix does exactly that, checks the join
result, and on timeout deliberately leaves the thread object and the handle alone: a leaked
`QThread` is a far smaller fault than either of the above, and the log says why.

Verified with real sessions: the monitor suite starts and stops ETW sessions 118 times' worth of
assertions with no failures, and `logman query -ets` shows **no Flowbit session left behind**.
`test_etw_teardown` asserts the three-step order and the guarded delete (9 checks).

## Checked and sound in pass 4

| Item | Evidence |
|---|---|
| `OpenProcess` handles | every site opens, uses, closes on all paths; `sweepDeadPids` closes before erasing |
| Shell icons | `SHGetFileInfoW` result's `HICON` is `DestroyIcon`'d immediately, and `pruneIconCache` bounds the cache |
| WinDivert handles | startup failure paths close the flow handle before returning; `shutdown()` closes both and nulls them; workers publish their handle so teardown can shut it down |
| Worker threads | `flowThread`/`netThread`/`tableThread` joined in `shutdown()`; throttle workers `join()`ed after `WinDivertShutdown(RECV)` unblocks them |
| Updater | `QNetworkReply` deleted in its finished handler; staged `QFile` deleted on the failure path |
| Service handles | all `OpenSCManager`/`OpenService` pairs close before every return, including the repair path |

---

## Updated plan status

| Finding | Severity | Status |
|---|---|---|
| F1 unchecked packet injection | HIGH | fixed |
| F2 GUI blocked on schtasks | HIGH | fixed |
| F3 GUI blocked on monitor teardown | MEDIUM | fixed |
| F4 unchecked permissions | MEDIUM | fixed |
| F5 dead repair state | LOW | fixed |
| F6 warnings | LOW | fixed |
| F7 pacer data race | HIGH | fixed, tested |
| F8 unbounded rate cast, day mask | MEDIUM | fixed, tested |
| F9 unbounded history growth | LOW | fixed, tested |
| F10 ETW handle closed under ProcessTrace, running QThread deleted | HIGH | fixed, tested |

---

## Plan, in priority order (original, kept for the record)

1. **F1** — result-checked packet injection with bounded retry and a failure counter surfaced in
   diagnostics. This is the only finding that can silently break a user's connection.
2. **F2** — async task registration; no UI thread waits on an external process.
3. **F3** — `stopAsync()` for the monitor, used by the panel.
4. **F4** — checked permissions with an error path.
5. **F5, F6** — dead state and warnings.

Each fix lands with a test that fails on the old behaviour, a build verified by exit code, and the
full suite sweep (no suite may report zero checks).
