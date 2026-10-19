# RESOLVED: per-application direction (download vs upload)

**Status: fixed, deployed, and confirmed by the user on 2026-10-16 at 20:04.**

## Confirmation

The user's screenshot after the fix, while downloading in Firefox:

| | Download | Upload |
|---|---|---|
| Flowbit: Mozilla Firefox | **127 KB/s** | 4 B/s |
| Flowbit: Live Speed (whole system) | 147 KB/s | 5.42 KB/s |
| Flowbit diagnostics | `ETW events received: 12009 (decoded: 11969)` | |

Firefox now carries the bulk of the download, upload is near zero, and the
per-application figure is consistent with the system-wide figure. Previously the
same activity showed `Download 622 B/s, Upload 142 KB/s` - inverted.

## What the bug was

One inverted assumption, held for several rounds:

* The code mapped **event id 10 to receive and 11 to send**.
* The truth is **id 10 = send (upload), id 11 = receive (download)**.

## What proved it

A live capture during a 3.4 GB Firefox download (`Phase 0.1` raw dump, reported at
`2026-10-16T19:40`):

```
id=10  total 1,165 bytes, largest   660   -> requests and ACKs  = UPLOAD
id=11  total 20,763 bytes, largest 10,080 -> the file data      = DOWNLOAD
```

Plus the second finding that invalidated three earlier attempts:

```
id=11 saddr=0x8f00a8c0 ... saddr_net=192.168.0.143 saddr_is_local=yes
```

**The provider publishes the LOCAL endpoint in `saddr` in both directions.** Every
captured event - send and receive - had `saddr_is_local=yes`. So an address
comparison can never discriminate direction, and all the code that tried to use
one always fell through to a fallback. Those fallbacks are what inverted the
figures.

## The fix

* **Direction is decided solely by the event id** (`10 = send`, `11 = receive`,
  `14` retransmit as send, `26/27/30` the IPv6 equivalents).
* **The address and port heuristics were deleted**, not kept as fallbacks. Keeping
  them was the bug: once the id decides correctly they can only produce a wrong
  answer.
* **Four regression tests** now pin the behaviour (the appmon suite went from 55 to
  59 checks), encoding the rule so a future change cannot silently invert it again.

## The process lesson, recorded

Every earlier attempt failed the same way: a plausible rule was implemented
**without first capturing a case of known direction**. That produced three wrong
fixes (an invented "even id = send" rule, a hardcoded IANA ephemeral-port range
that does not match this machine's 1024-15000, and a byte-order correction applied
without logging the raw value). The working fix came from logging raw facts first
and reading the answer off them.

## Remaining work

* **Per-application bandwidth limiting has still never been verified to throttle.**
  That is the last untested feature and the subject of Phase 3 in
  `roadmap-direction.md`.
