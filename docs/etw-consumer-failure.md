# Why ETW delivers nothing, and what Flowbit now does about it

Symptom reported three times on the same machine:

```
Leftover trace session   active (this session)
ETW events received      0 (decoded: 0), 40s
Event ids seen           none yet
UDP events counted       0
```

A trace session that exists, is owned by Flowbit, reports itself active — and delivers nothing. The
panel then falls back to the TCP counter source and shows its note ("TCP only - UDP and QUIC are not
counted"), which is correct for that source but was being read as "Flowbit cannot count UDP".

## What the documentation actually says

From Microsoft's **Consuming Events** (Event Tracing):

> To consume events from a real time session, set the **LoggerName** member to the session name.
> […] you can process events from one or more log files, but from only **one real-time session**.
> […] The **ProcessTrace** function blocks the thread until your consumer processes all events in
> the trace sessions, the *BufferCallback* returns **FALSE**, or you call **CloseTrace**.

Two consequences drive everything below:

1. **ProcessTrace returning is an event in itself.** For a live session it should return only when
   the session is stopped or the trace handle is closed. If it returns while the session is still
   wanted, the consumer is dead and everything the provider sends from then on is discarded — while
   the session still looks "active".
2. **One consumer per real-time session.** A second `OpenTrace` on the same session name cannot
   help; it competes.

## What was wrong in Flowbit (three separate faults)

### 1. ProcessTrace's return code was discarded

```cpp
thread = QThread::create([this] {
    PROCESS_TRACE_MODE_EVENT_RECORD;                    // stray no-op statement
    ProcessTrace(&traceHandle, 1, nullptr, nullptr);    // result thrown away
});
```

A consumer that failed immediately left the session open and owned, with nothing reading it. The
application could not tell "no traffic" from "no consumer". **Fixed**: the code is captured, the exit
is flagged, the process-wide consumer slot is released, and Diagnostics reports
`stopped (ProcessTrace N)`.

### 2. The provider was enabled *before* the consumer existed

Order was: `StartTrace` → `EnableTraceEx2` → `OpenTrace` → `ProcessTrace`. The session accepts the
enable straight away, buffers begin to fill, and no consumer is draining them. On this machine the
result was a session that stayed silent for as long as it ran. **Fixed**: the provider is enabled
*after* the consumer thread is running, and the enable's result is recorded
(`enable ok / enable 5, attempt 1`).

### 3. Nothing retried, and nothing reported

There was no recovery path and no visible state, so a silent session was indistinguishable from a
working one. **Fixed**: one re-enable is issued if the session is still silent after 5 seconds
(recorded as `re-enabled after 5s`), a dead consumer is rebuilt up to twice per run, and the
`ETW consumer` row in Diagnostics carries the state.

## What the app reports now

Diagnostics → Monitoring:

| Row | Example | Meaning |
|---|---|---|
| `ETW consumer` | `running · enable ok, attempt 1` | Consumer alive, provider enabled |
| `ETW consumer` | `stopped (ProcessTrace 87), 1 restart(s) · enable ok, attempt 2` | Consumer died; code captured; rebuilt once |
| `ETW consumer` | `running · enable 5, attempt 1` | Enable refused (5 = access denied) — the fallback is in use |
| `Event ids seen` | `11:400 10:80 43:40` | What actually arrived, per id |
| `UDP events counted` | `40` | UDP ids 42/43/58/59 seen |

## How to read a failing report

- **`stopped (ProcessTrace N)`** → the consumer returned; N is the reason. Raise N here.
- **`enable N`** where N ≠ 0 → the provider refused the enable; the session itself is fine.
- **`enable ok` + `none yet` + traffic flowing** → the enable was accepted and still nothing arrived.
  That is the remaining unknown state, and it is the one that needs WPR on the machine:
  `wpr -start GeneralProfile -start Network` for 10 seconds, then compare with Flowbit's rows.


## QUIC is not a separate case

QUIC - and HTTP/3, which most video sites now use - runs **over UDP**, normally to port 443. There is
no separate QUIC provider, no separate counter and nothing extra to decode: a QUIC stream appears in
this provider as UDP send/receive events (ids 42/43, or 58/59 on IPv6), attributed to the owning
process like any other traffic.

So the relationship is strictly one-way:

  * UDP events arriving  -> QUIC is counted, for free
  * UDP events absent    -> QUIC is absent too, and no amount of QUIC-specific work changes that

That is why the fallback note reads "TCP only - UDP and QUIC are not counted": the TCP counter source
has no visibility of either, and both return together.

## Files changed for this

| File | Change |
|---|---|
| `src/appmonitor.cpp` | capture `ProcessTrace` result; enable provider after the consumer attaches; one-shot re-enable at 5 s; rebuild a dead consumer; `consumerReport()` |
| `src/appmonitor.h` | `consumerReport()` declaration |
| `src/diagnostics.cpp` / `.h` | `ETW consumer` row + `consumerState` parameter |
| `src/diagnosticspanel.cpp` / `.h` | plumb the consumer state through |
| `src/mainwindow.cpp` | supply `m_appMonitor->consumerReport()` |
| `tools/test_diagnostics_etw.cpp` | a stopped consumer is a Warning carrying its code; a running one is OK |

## What is still not proven

The ordering fix is the documented, principled change — but **it has not been observed fixing this
machine**, because a live session can only be created by the elevated application, not from the
audit environment. If `ETW consumer` reads `running · enable ok` and `Event ids seen` still says
`none yet` after a minute of real traffic, the remaining cause is outside Flowbit's session handling
(provider blocked by policy, or events not emitted on this build), and the WPR capture above is the
next step rather than another guess.
