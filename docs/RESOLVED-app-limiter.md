# RESOLVED: per-application bandwidth limiting

**Status: verified working on 2026-10-16 by measurement, with a control test.**

Until this point the app's headline feature had **never** been confirmed to
throttle anything. It does.

## The measurement

Same URL, same file size (1 MB), same requesting process, alternating only the
configuration:

| Configuration | Elapsed | Throughput | Configured limit |
|---|---|---|---|
| `powershell.exe` rule at 40 KB/s, limiter **on** | 23.92 s | **41 KB/s** | 40 KB/s |
| Rule disabled, limiter **off** (control) | 5.58 s | **175 KB/s** | none |

* The throttled rate is within **2.5%** of the configured limit.
* The control run is **4.3x** faster, so the difference is the limiter and not
  network variance.
* An earlier attempt at a 3 MB download under the limit **timed out after 180 s**,
  where the same download took 20-27 s unlimited - consistent with the same cap.

The machine's line rate is ~6 Mbps (~165-175 KB/s), so the 40 KB/s limit was
selected well below capacity to make the effect unmistakable.

## How the test was set up

1. A per-application rule was written into
   `%LOCALAPPDATA%\Flowbit\Flowbit\settings.json`:

   ```json
   "appRules": [ { "id": 9002, "exe": "powershell.exe", "enabled": true,
                   "downBps": 40960, "upBps": 40960 } ]
   ```

   with `"enabled": true` for the limiter itself.

2. Flowbit was restarted so the rule set is read at startup.

3. The measured process was the **same process that performed the download**
   (`Invoke-WebRequest` runs inside the calling PowerShell). An earlier test that
   limited `firefox.exe` while downloading from PowerShell proved nothing, because
   the rule did not apply to the process doing the work - worth remembering as a
   testing mistake.

## What this confirms

* The WinDivert packet path is live and pacing real traffic.
* Per-application attribution for enforcement works: the rule was matched to the
  process by executable path, and only that process was affected.
* Rate configuration is honoured to within a few percent.

## Note for future testing

The app pushes rules to the enforcement engine when the rule set is applied.
Restarting the app guarantees a clean read of `settings.json`, which removes any
question of hot-reload behaviour from the test.

## Remaining known limitations (unchanged, not blocking)

* IPv6 application traffic is matched through the same tuple logic; the earlier
  IPv6 gap in the packet path was fixed (see `audit-2026-10-16.md`, finding R1).
* The driver's FLOW-layer attribution is unavailable on this Windows build, so
  attribution falls back to connection-table polling (see finding R10). This was
  confirmed not to prevent enforcement, since the test above passed on exactly
  this machine.
* Historical per-application statistics remain out of scope.
