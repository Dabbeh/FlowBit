# Roadmap: fixing upload/download direction in per-application monitoring

## What the issue actually is (confirmed, not assumed)

The panel reports **receive traffic as upload**. Firefox downloading a video shows
`Download 622 B/s, Upload 142 KB/s` while Windows Resource Monitor shows that same
traffic as *receive*.

### The proven chain of cause

1. **The provider has no direction field.** Windows' own schema for
   `Microsoft-Windows-Kernel-Network` ids 10/11 is exactly:

   ```
   id 10: PID, size, daddr, saddr, dport, sport, startime, endtime, seqnum, connid
   id 11: PID, size, daddr, saddr, dport, sport, seqnum, connid
   ```

   There is no `direction`, `send`, or `recv` property. **Direction can only come
   from the addresses** (`saddr`/`daddr`) or be inferred from the two ids.

2. **The address comparison never matched.** A captured event reported
   `saddr = 143.0.168.192`, while the machine's Wi-Fi address is `192.168.0.143`:
   the same address with its bytes reversed. Verified numerically:

   ```
   143.0.168.192  reversed  ->  192.168.0.143   MATCH
   ```

   `isLocalAddress()` compared against `192.168.0.143` and failed every time, so
   the address test could never decide direction.

3. **The fallback inverted it.** With the address test unable to decide, direction
   fell back to `id 10 = send`. That is an assumption with no documentation behind
   it, and it produced the observed inversion: downloads counted as uploads.

4. **A byte-order fix was deployed and appeared not to help.** That means one of
   the assumptions in step 2 or 3 is still wrong, or the fix did not take effect
   in the running build.

## What must be established before writing more code

### Fact A: the real byte order of `saddr`/`daddr`

Two candidate interpretations, and the capture is consistent with the *second*:

| interpretation | value for the machine at 192.168.0.143 | matches capture? |
|---|---|---|
| little-endian words as stored | `143.0.168.192` | yes, if that is the raw order |
| network order | `192.168.0.143` | yes, if reversed |

The capture proves the two differ; it does not yet prove which one the *decoder*
should use, because the app never logged the raw integer alongside the formatted
address. **Action: log the raw uint32 hex value plus both formatted
interpretations, so the ambiguity is removed by inspection.**

### Fact B: which of `saddr`/`daddr` is local for a receive

For a download, the remote server is the sender, so the local address should be
`daddr`. For an upload it should be `saddr`. **Action: capture a transfer whose
direction is known with certainty** - a download from a known remote IP - and log
both addresses raw. Whichever equals the machine's own address tells us which
field holds the local endpoint, and whether the app's rule `send = isLocal(saddr)`
is the right way round.

### Fact C: whether ids 10/11 are really send/receive

If Fact B shows the address rule is correct but the ids are swapped relative to
expectation, the fallback ordering is what needs changing. **Only touch this after
A and B are settled**, because changing it on a guess is what produced the last
three failed attempts.

## The roadmap

### Phase 0 - stop guessing (do this first)

- [ ] **0.1** Add a raw-value dump: for each of the first N non-loopback events,
      record `saddr` and `daddr` as `0x%08X` **and** both formatted orders, plus
      `sport`/`dport` and the id. No decision, no interpretation - raw facts only.
- [ ] **0.2** Capture one **download** from a known remote host and one **upload**
      to a known remote host, with the machine's own addresses listed alongside.
- [ ] **0.3** Derive the correct rule from those captures and write it down in
      `docs/etw-kernel-network-schema.md` before implementing it.

### Phase 1 - implement the rule proven by Phase 0

- [ ] **1.1** Local-address comparison that matches the real byte order (and is
      order-tolerant as a safety net, since both orders have now been observed).
- [ ] **1.2** Direction decided by: local endpoint first (addresses), then ports
      using the machine's **real** dynamic range (already read from the registry -
      this machine is 1024-15000, not the IANA 49152+), then the id mapping only
      as a last resort.
- [ ] **1.3** A unit test that encodes the Phase 0 capture as data, so the rule is
      pinned by a test rather than by memory.

### Phase 2 - verify against an independent source

- [ ] **2.1** The only trustworthy cross-check is Windows' own per-process view.
      Resource Monitor reads the same provider, so a match is meaningful.
- [ ] **2.2** As a **second, independent** implementation of the same measurement,
      use `GetPerTcpConnectionEStats` per connection (already implemented in the
      monitor as the TCP fallback) and compare its direction split against the ETW
      path. Two independent sources agreeing on direction is strong evidence; they
      cannot share the same bug because one uses connection-table statistics and
      the other uses event payloads.
- [ ] **2.3** Ship only when both agree with Resource Monitor.

### Phase 3 - the untested feature

- [ ] **3.1** Per-application bandwidth limiting has **never** been verified to
      throttle. Test it independently of any of the above: set a low limit, run a
      sustained download, measure with an external means (Resource Monitor or a
      speed test), and confirm the throughput obeys the limit.

## Definition of done

1. Firefox downloading a video shows a **large Download** and small Upload, and
   both agree with Resource Monitor within a reasonable margin.
2. An upload-dominant activity (a file upload or a speed test's upload phase)
   shows the opposite split.
3. Idle applications show ~0 in both directions.
4. A per-application limit measurably caps throughput.

## Why the previous attempts failed

Three changes were made on plausible reasoning rather than proof: an invented
"even id = send" rule, a hardcoded IANA port range that does not match this
machine, and a byte-order fix applied without logging the raw value first. Each
was reasonable, none was verified against a capture of known direction. **Phase 0
exists specifically to remove that failure mode.**
