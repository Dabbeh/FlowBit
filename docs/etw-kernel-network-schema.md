# Microsoft-Windows-Kernel-Network: the definitive event schema

This document exists because the per-application activity panel was wrong for
several rounds, and every one of those rounds was wasted on **assumptions** about
this provider instead of reading its published schema. It is now recorded from
the OS itself, so no future change has to guess.

## Where this data comes from

Not a web page, not a blog, not memory: the schema below was read **from Windows**
with `tools/etw_schema_dump.cpp`, which calls `TdhGetEventInformation` on the
provider registration. The provider is registered here:

```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\WINEVT\Publishers\
    {7dd42a49-5329-4832-8dfd-43d979153a88}
        (default)        = Microsoft-Windows-Kernel-Network
        ResourceFileName = C:\WINDOWS\system32\Microsoft-Windows-System-Events.dll
        Enabled          = 1
```

The GUID above is the authoritative provider id. Run the dump tool again on any
machine to re-derive the whole table (`etw_schema_dump.exe <out.log>`, elevated).

## The events we care about

| ID | Meaning | Properties |
|----|---------|-----------|
| **10** | TCP send (IPv4) | PID, size, daddr, saddr, dport, sport, startime, endtime, seqnum, connid |
| **11** | TCP receive (IPv4) | PID, size, daddr, saddr, dport, sport, seqnum, connid |
| **12** | TCP connect/accept (IPv4) | PID, size, daddr, saddr, dport, sport, mss, sackopt, tsopt, wsopt, rcvwin, rcvwinscale, sndwinscale, seqnum, connid |
| **13** | TCP disconnect (IPv4) | PID, size, daddr, saddr, dport, sport, seqnum, connid |
| **14** | TCP retransmit (IPv4) | same shape as 11 |
| **15** | TCP re-connect (IPv4) | same shape as 12 |
| **16** | TCP copy (IPv4) | same shape as 11 |
| **17** | TCP failure | Proto, FailureCode |
| **18** | TCP ... (IPv4) | same shape as 11 |
| **26–32** | the IPv6 equivalents | same, but **daddr/saddr are `Binary`** |

### The critical facts

1. **`PID` is a top-level property (UInt32)** — not only in the event header.
   Read `PID` from the payload; it is the traffic owner. The header's
   `EventHeader.ProcessId` is *not* the same thing for these events and must not
   be used for attribution.

2. **`size` is a top-level `UInt32`** named exactly `size`. Measured field name in
   the schema; a name match on `size` is correct **for this provider**.

3. **Direction is NOT encoded in the event ID.** There is no documented "even id =
   send" rule, and inventing one inverted download and upload in practice. Look at
   the pair `saddr` / `daddr` and decide: the operation is a **send** when `saddr`
   is one of this machine's own addresses, a **receive** when `daddr` is.

4. **`daddr`/`saddr` are `UInt32` in the IPv4 events (ids 10–18)** and
   **`Binary` in the IPv6 events (ids 26–32)**. A decoder must branch on the
   declared type, not assume 4-byte integers. The IPv4 values are in network byte
   order (`out=HexInt64` is only a display formatting hint).

5. **Watch out for the other integer properties.** Events 12/15/28/31 carry
   `mss`, `sackopt`, `tsopt`, `wsopt`, `rcvwin`, `rcvwinscale`, `sndwinscale`,
   `seqnum`, `connid` — all UInt16/UInt32. Any decode rule of the form "take the
   first integer property as the byte count" is therefore **wrong** and will pick
   `PID` (property 0) or a TCP option field. The name `size` must be matched
   explicitly.

6. **We only count ids 10, 11, 12, 13** (TCP send/receive, UDP send/receive in the
   classic interpretation) — but note that in this schema 12/13 are TCP
   connect/disconnect, whose `size` is typically 0. The byte-carrying events are
   **10 and 11**. Treating 12/13 as "UDP" (as earlier code comments claimed) is
   wrong for this provider revision.


## UDP: the missing half (researched, then confirmed against Microsoft's own mapping)

This document originally recorded only TCP ids, and warned that ids 12/13 are TCP
connect/disconnect rather than UDP. That was correct, and it left UDP uncounted - so DNS, QUIC,
WireGuard, game traffic and most video calls never appeared in the application list, while Windows'
own Resource Monitor (same provider) showed them.

The ids are not in any header shipped on this machine and the provider manifest is not on disk. They
are, however, in Microsoft's own generated mapping of this exact provider:

  github.com/microsoft/Tx  ->  Generated/Microsoft_Windows_Kernel_Network.cs
  provider {{7dd42a49-5329-4832-8dfd-43d979153a88}}

| Id | Event | Manifest message | Direction |
|---|---|---|---|
| 42 | KNetEvt_SendIPV4Udp | "UDPv4: %2 bytes transmitted ..." | UDP send |
| 43 | KNetEvt_RecvIPV4Udp | "UDPv4: %2 bytes received ..." | UDP receive |
| 58 | KNetEvt_SendIPV6Udp | "UDPv6: %2 bytes transmitted ..." | UDP send |
| 59 | KNetEvt_RecvIPV6Udp | "UDPv6: %2 bytes received ..." | UDP receive |
| 49 | KNetEvt_FailUdp | "UDPv4: Connection attempt failed ..." | no bytes |

A second lesson from the same file: the manifest does NOT name its events "KNetEvt_UdpSend". The
symbols are `KNetEvt_SendIPV4Udp` and the messages are `Data_sent_over_UDP_protocol_` - lowercase
"UDP" and the verb "sent", not "Send". A matcher looking for "Udp" and "Send" case-sensitively
misses the message form. The classifier therefore compares case-insensitively and accepts both
"send"/"sent" and "recv"/"receiv". Ids remain the primary rule; names are the fallback for builds
whose ids differ.

Verification available at runtime: Diagnostics has a "UDP events counted" row. Zero there while UDP
traffic is flowing means the events are not arriving on this build, which is a different fault from
being miscounted.

## What went wrong in the app (recorded so it is not repeated)

| Symptom | Root cause | Fix |
|---|---|---|
| Panel always empty | decode loop accepted only struct-typed properties; `size` is a top-level scalar | scan all top-level properties |
| Panel empty despite a running session | `completeInterval()` existed but was never driven from the UI | `tick()` called from the 1 s dashboard timer |
| Only ~40% of events decoded (28 646 / 72 470) | decoder matched the literal name `size` only in some paths, and the offset fallback read payload byte 0 (which is `PID`) | match `size` by name; type-based fallback only when exactly one integer property exists |
| Download/upload inverted (45 KB/s down vs 272 KB/s up while downloading) | direction taken from event-ID parity — an invented rule | direction decided from `saddr`/`daddr` against the machine's own addresses |
| Idle processes shown with traffic, Firefox missing | attribution used `EventHeader.ProcessId` | read the payload `PID` property |
| Zero events on Windows build 26300 | enable level `TRACE_LEVEL_INFORMATION` filtered out level-5 events | enable at `TRACE_LEVEL_VERBOSE` with the full keyword mask |

## Live verification status

**Ground truth that works**: Windows' own Resource Monitor, reading this same
provider, showed for a YouTube session:

```
firefox.exe   PID 32608   Send 2,830 B/s   Receive 391,445 B/s   Total 394,276 B/s
```

That is the target: **Receive ≫ Send** for video streaming, and the PID is the
real browser process.

**A note on the standalone verifier** (`tools/etw_live_decode.cpp`): it starts a
session, enables the provider at VERBOSE with all keywords, and consumes in real
time, yet reports `seen=0` — both when the provider is enabled before the consumer
attaches and after. The application, using the same provider GUID and the same
enable call, receives tens of thousands of events (measured: `received 72470`).
The discrepancy is unresolved and is recorded here rather than papered over; the
application's own counters are therefore treated as the trustworthy measurement,
and the app's diagnostics report is the verification surface of record.

## Verification commands

```powershell
# re-derive the schema on any machine (elevated)
build\probe\etw_schema_dump.exe kernel_network_schema.log

# live counters, after generating traffic, in the app:
#   Settings -> Diagnostics -> Save Report
#   -> "ETW events received: N (decoded: M)"  (M should be ~N)
#   -> "ETW event layout" lists every id with its fields
```
