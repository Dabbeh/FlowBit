# Per-Application Network Monitoring — Feasibility Findings

Investigated before writing any feature code, per the "validate the backend first"
instruction. Findings are from probes in `tools/probe_appmon*.cpp`, run on this
machine.

## Summary

Per-process byte attribution on Windows needs **Administrator rights**. There is
no supported unelevated API that reports real per-process traffic volume. This is
a platform constraint, not an implementation choice.

Two mechanisms were evaluated and both were tested, not assumed.

## Mechanism 1 — ETW (`Microsoft-Windows-Kernel-Network`)

The correct tool for this job: its events carry a PID and byte counts.

**Result: unavailable without elevation.**

```
logman create trace ... -p Microsoft-Windows-Kernel-Network
  -> Error: Access is denied
```

Creating any ETW trace session requires membership in **Administrators** or
**Performance Log Users**. This account holds Administrators as
`Group used for deny only`, so *every* provider fails, including
`Microsoft-Windows-Kernel-Process` and `Microsoft-Windows-Kernel-File`.

## Mechanism 2 — Per-connection TCP EStats

The documented unelevated-looking route:

1. `GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)` — connection list **with PIDs**
2. `SetPerTcpConnectionEStats(..., TcpConnectionEstatsData, EnableCollection=TRUE)`
3. `GetPerTcpConnectionEStats(...)` — per-connection `DataBytesIn` / `DataBytesOut`

Steps 1 and 3 work unelevated. **Step 2 does not.**

```
pid=6052  set=5(FAIL) get=0(ok)
pid=32552 set=5(FAIL) get=0(ok)
6 checked, 0 enable-ok, 6 read-ok
```

`5` is `ERROR_ACCESS_DENIED`.

### The dangerous part

`GetPerTcpConnectionEStats` returns `NO_ERROR` **even when collection was never
enabled**, and writes nothing to the ROD buffer. Because the buffer is a stack
local, it is read back as uninitialised memory.

The first probe was fooled by this: it reported "39 connections with readable
counters" and plausible-looking rates, and even a 580 KB/s figure for the process
running a download. All of it was noise. The tell was identical values repeated
across unrelated PIDs:

```
pid=6052   DataBytesIn = 28429462281519221
pid=19804  DataBytesIn = 28429462281519221    <- same number, different process
pid=16308  DataBytesIn = 28429462281519221
```

A second tell was upload figures of ~1e15 KB/s, which is what an uninitialised
64-bit value looks like when divided by an interval.

**Lesson for the implementation:** a `NO_ERROR` return from these APIs is not
evidence that a counter was measured. The only trustworthy signals are the
`Set...` return code and the RW `EnableCollection` acknowledgement.

## What this means for the feature

The Flowbit executable already requests `requireAdministrator` in its manifest,
because WinDivert needs it to shape traffic. So **at runtime the application is
elevated and ETW will work.** The blocker is limited to this development shell,
which cannot elevate.

Consequences:

- The monitoring backend can be written against ETW.
- **It cannot be validated from here.** Any claim that it produces correct
  per-process numbers would be unverified.
- The UI must therefore be built to report the real state of the backend rather
  than assuming it works: "unavailable" and "no traffic" are distinct, and a
  failed session must never render as a list of zeroes.

## Decision

Implement the backend against ETW with a runtime capability check, and make the
UI's failure state a first-class path. Do not fabricate values and do not present
uninitialised counters as measurements.
