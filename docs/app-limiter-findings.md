# FLOWBIT — Per-Application Bandwidth Limiting: Feasibility Findings

Investigated before writing feature code, per the Phase 1 instruction "do not begin
the complete UI implementation until real per-application bandwidth enforcement has
been demonstrated."

Probes: `tools/probe_applimit.cpp` (attribution), `tools/applimit_poc.cpp`
(enforcement). Both were compiled and their non-elevated paths exercised here.

## Summary

Per-application enforcement **is achievable** with the WinDivert driver Flowbit
already ships and loads. The approach requires an architectural detail that is easy
to get wrong, and it requires Administrator rights, which Flowbit already holds.

What has been **verified here**: the API surface exists, the driver is running, the
attribution fields are present in the address struct, the POC compiles and correctly
refuses to run unelevated.

What has **not** been verified here: actual enforcement against live traffic, because
starting a WinDivert handle returns `ERROR_ACCESS_DENIED` for a non-elevated process,
and this development account holds Administrators as "Group used for deny only".

## The critical constraint: ProcessId is not available at the packet layer

WinDivert's filter language and `WINDIVERT_ADDRESS` do carry a `ProcessId`, which
makes a per-process packet filter look straightforward. It is not.

From the bundled WinDivert 2.2.2 documentation:

> The `Flow.*` fields are only valid at the `WINDIVERT_LAYER_FLOW` layer.
> The `Socket.*` fields are only valid at the `WINDIVERT_LAYER_SOCKET` layer.

`WINDIVERT_DATA_NETWORK`, the struct used at the layer where packets can actually be
delayed, contains only `IfIdx` and `SubIfIdx`. There is no process information.

So a filter such as `outbound and processId == 1234` cannot be expressed at the
network layer. Writing one would compile and then match everything or nothing.

## The workable approach

Two layers, combined:

1. **FLOW layer** (`WINDIVERT_LAYER_FLOW`). Each flow event carries `ProcessId` and
   the flow's local/remote address and port, plus protocol. This is where a process
   is identified.
2. **NETWORK layer** (`WINDIVERT_LAYER_NETWORK`). This is where packets are held and
   re-injected, which is what actually shapes traffic.

The join between them is the **5-tuple**. Flows whose owning process matches a
configured application are recorded as a tuple set; at the network layer each packet's
tuple is looked up in that set and paced only on a match.

Two details make this correct rather than approximately correct:

- **Direction normalisation.** An outbound packet has the local endpoint as its
  source; an inbound packet has it as its destination. Normalising both to
  "local endpoint, then remote endpoint" lets a single flow record match packets in
  both directions.
- **Continuous re-attribution.** The flow thread keeps running and re-inserts tuples,
  so an application that starts after Flowbit does is picked up when it launches, and
  a restarted application re-registers its new tuples automatically.

Because matching is by tuple, two applications can hold different limits at the same
time without interfering: their tuple sets are disjoint.

## Why this composes with the existing system-wide limiter

The existing `ThrottleEngine` already runs a network-layer handle with a token bucket
for the system-wide cap. The per-application limiter is a second, independent pace
applied to a subset of packets.

Ordering matters: a packet belonging to a limited application passes through both
buckets, so the tighter limit dominates, and the system-wide cap stays enforced for
everything else. Neither bucket can disable the other. This is the behaviour the brief
asks for in section 6.

## Elevation

`WinDivertOpen` for both FLOW and NETWORK returns `ERROR_ACCESS_DENIED` (5) from a
non-elevated process. Flowbit's manifest already requests `requireAdministrator`
because the existing limiter needs it, so no new privilege handling is required.

No kernel driver is installed by the feature: WinDivert.dll and WinDivert64.sys are
already shipped and loaded by Flowbit, and the driver is registered once by the
existing application startup. The feature adds no new driver deployment.

Note for the record: on this machine the WinDivert service is already present and
`RUNNING`, confirming the existing startup path works.

## Include-order hazard (found while building the probe)

`windivert.h` defines `__in`, `__out` and `__inout` as **empty** macros for non-MSVC
compilers. libstdc++ uses identifiers of that exact shape internally - for example
`std::move(__in)` in `<bits/stl_pair.h>` - so including `windivert.h` **before** the
C++ standard headers rewrites standard-library internals into calls with no arguments:

```
bits/stl_pair.h:1041: error: no matching function for call to 'move()'
```

The fix is to include the standard headers first and `windivert.h` last. Any new
translation unit that uses `<algorithm>`, `<map>` or `<thread>` alongside WinDivert
must follow that order. This was isolated with a minimal repro and is not a
misdiagnosis.

## Performance posture

The brief warns about latency during gaming, which rules out a naive design:

- The flow thread and the network thread each do bounded work per event; no process
  enumeration happens on a timer, and no packet is copied.
- Process image paths are resolved only when a flow event arrives for a process not
  already known, and are cached.
- The token bucket computes its wait and sleeps until the refill instant rather than
  polling, so an idle limiter costs no CPU.
- Per-packet work is one set lookup. Unlimited applications are skipped before any
  bucket is consulted.

## What remains unproven, and how it will be proven

The measured-throughput requirement from section 9 cannot be satisfied from this
shell. `applimit_poc.exe` was built and packaged for an elevated run. It prints
per-second paced throughput against the configured limit and the processes it
attributed traffic to, so enforcement can be confirmed by comparing its figures with
the target application's own reported speed.
