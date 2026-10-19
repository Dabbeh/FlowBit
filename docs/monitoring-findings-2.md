# Per-application monitoring: verified findings and remaining defect

Companion to `etw-kernel-network-schema.md`. This records what has been **measured**
on the user's machine (Windows 11 build 26300) so the remaining work is derived
from evidence rather than assumption.

## The ghost-session mystery — SOLVED

An ETW session named `Flowbit-AppMonitor` existed that:

* made `StartTrace` fail with `ALREADY_EXISTS` (so the app's monitoring could not start),
* was invisible to `logman` ("Data Collector Set was not found"),
* and could **not** be stopped: `ControlTrace(STOP)` returned `0` (success) and a
  follow-up `QUERY` still returned `0` (present) instead of `4201` (not found).

Verified with `tools/controltrace_probe.cpp`:

```
QUERY 'made-up-name'      -> 4201   (absent: the real "not found" code)
QUERY 'Flowbit-AppMonitor' -> 0      (present)
STOP  'Flowbit-AppMonitor' -> 0      (reported success)
QUERY after stop          -> 0      (STILL present!)
```

**Cause: live ETW consumers were holding the session open.** Failed runs of the
self-test / probe tools did not exit, so their `ProcessTrace` loops stayed alive
and kept the session pinned. At the peak there were **92 leftover
`appmon_selftest.exe` processes** on the machine. A session with a live consumer
cannot be reclaimed, which is why `STOP` "succeeded" yet nothing went away.

Once every consumer was killed, the same stop returned the correct result:

```
STOP rc=0 ; QUERY after stop rc=4201   <- gone
```

Consequences that were previously misdiagnosed as decoder bugs:

* the app's `eventsSeen` figure (e.g. 72 470) came from a session the app could
  not control, so its numbers were unreliable;
* a standalone verifier saw `seen=0` while the app saw events - the ghost session
  already owned the provider subscription;
* the "empty panel" episodes were monitoring failing to start at all.

**Lesson recorded:** every tool that opens a trace consumer must exit on its own;
a hung consumer poisons the provider name for every later run until it is killed
or the machine reboots.

## Direction (send vs receive) — measured behaviour

The published schema gives `saddr`/`daddr` per event. The runtime samples show:

```
id=11 saddr=0x0100007f daddr=0x0100007f sLocal=1 dLocal=1 knownLocal=9
id=10 saddr=0x0100007f daddr=0x0100007f sLocal=1 dLocal=1 knownLocal=9
```

* `0x0100007f` is **127.0.0.1** in the same byte order as `sin_addr.s_addr`, so the
  comparison against locally enumerated addresses is correct as written.
* These samples are **loopback** traffic: both endpoints are local, so the address
  test legitimately cannot decide direction, and the documented id mapping is
  used instead (10 = send, 11 = receive).
* No sample of a genuine internet connection was captured, because the loopback
  events dominate by volume. A real-traffic capture (browsing, with no local
  generator running) is still required to confirm the internet case.

The earlier inverted readings (a download reported as an upload) are explained by
the ghost session rather than by the direction rule: the data being displayed came
from a session the app did not own.

## FIXES VERIFIED (clean state, occupied-name test)

Three defects fixed and **demonstrated**, not asserted:

### 1. Monitoring no longer refuses to start when the session name is taken

Reproduced the exact failure by creating a real trace session named
`Flowbit-AppMonitor` with `logman create trace ... -ets` (verified present by
`logman query -ets`), then ran the self-test:

```
monitor.start() -> true
state: Running
```

Before the fix this printed *"A monitoring session from an earlier Flowbit run is
still active and could not be cleared automatically."* The start path now falls
back to a unique session name (`Flowbit-AppMonitor-<pid>`) when the shared name is
held by an orphaned session, and every path (consumer open, both stops) resolves
the name actually used, so a fallback session cannot leak.

### 2. Decode rate: 34% -> ~100%

```
before:  [etw] seen=9270  decoded=3164
after:   [etw] seen=33    decoded=33
         [etw] seen=247   decoded=247
         [etw] seen=765   decoded=763
```

Cause: `eventsSeen` was incremented for **every** event the provider emitted,
including connect/disconnect control events that carry no bytes and are
deliberately not counted. The counter now increments only for the transfer ids we
actually account for, so the ratio reflects real decode success. `bytes <= 0`
events (legitimately size 0 on control ids) are excluded from both sides.

### 3. The fork chain that created the ghost sessions

The self-test binary did not handle `--clear-monitor-session`, so when
`startSession` spawned it to clear a leftover, the child re-entered the whole
test, found the same leftover, and spawned another child - **95 processes**, each
holding a live ETW consumer, pinning the session so `STOP` returned success while
the session survived. The self-test now honours the flag and exits immediately:

```
leftover processes: 0
```

## Direction: measured, and the test was the weak link

Every captured address sample was loopback:

```
id=11 saddr=0x0100007f daddr=0x0100007f sLocal=1 dLocal=1 knownLocal=9
```

`0x0100007f` = 127.0.0.1 in `sin_addr.s_addr` byte order, so the local-address
comparison itself is correct, and 9 local addresses are known. But **both**
endpoints being local means the address test legitimately cannot decide, and the
documented id mapping (10 = send, 11 = receive) is used instead.

That is why a *download* appeared as an *upload* in these runs: the self-test's
own generator connects over loopback, so its traffic hits the id fallback and the
ids did not line up with the generator's roles. **This says nothing about real
internet traffic**, where exactly one endpoint is remote and the address test
decides correctly.

Confirming that requires a capture with genuine internet traffic and no local
generator running - which is the outstanding verification step.

