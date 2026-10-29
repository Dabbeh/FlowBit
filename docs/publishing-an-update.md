# Publishing an update

Flowbit checks for updates by reading one small file over HTTPS:

```
https://raw.githubusercontent.com/Dabbeh/FlowBit/main/docs/version.json
```

Publishing a release is three steps, and nothing else in the application needs changing.

## 1. Raise the version

Two places, and they must agree. The running version is what Flowbit compares against:

`src/main.cpp`

```cpp
QApplication::setApplicationVersion(QStringLiteral("1.1.0"));
```

`installer/flowbit.iss`

```
#define AppVersion "1.1.0"
```

If these disagree, Flowbit will offer an update to a version it is already running, which
then installs and offers itself again.

## 2. Upload the new executable

Attach the built `Flowbit.exe` to a GitHub release:

1. **Releases** → **Draft a new release**
2. Tag it `v1.1.0` - the tag name is not used by the updater, but keeping it equal to the
   version makes the release list readable
3. Attach `Flowbit.exe`
4. Publish

The URL in the next step is what the updater downloads, so the asset has to be named
`Flowbit.exe` exactly.

## 3. Edit `docs/version.json`

```json
{
  "version": "1.1.0",
  "url": "https://github.com/Dabbeh/FlowBit/releases/download/v1.1.0/Flowbit.exe",
  "sha256": "lowercase SHA-256 of Flowbit.exe",
  "notes": "What changed, in a sentence. Shown in the offer before anything is downloaded."
}
```

`sha256` is required whenever the version is newer. It is the SHA-256 of the exact
executable uploaded in step 2; the updater will not offer a version without one, and
will delete a download that does not match. Compute it from the built file:

```powershell
(Get-FileHash .\Flowbit.exe -Algorithm SHA256).Hash.ToLower()
```

Commit that to `main` after verifying the assets. With automatic checks enabled,
Flowbit 1.0.3 and later check five seconds after launch and every 30 minutes while
running. A newer version opens the update offer automatically, including when the
window is in the tray. Later postpones the next offer for 30 minutes; skipped
versions remain skipped. Earlier builds check on launch.

## Using `releases/latest/download` instead

The committed file points at:

```
https://github.com/Dabbeh/FlowBit/releases/latest/download/Flowbit.exe
```

That URL always resolves to the newest published release, so the file only has to change
when the **version number** changes - which it does every release anyway - along with the
**checksum** of the new executable. It is the simpler form and the one to prefer:

| | |
|---|---|
| `releases/latest/download/Flowbit.exe` | No edit needed when the asset is replaced |
| `releases/download/v1.1.0/Flowbit.exe` | Pins the exact file; useful if a release is replaced |

Whichever is used, the `version` field is what decides whether an update is offered. The URL
is only where to fetch it from once accepted.

## What the user sees

1. Flowbit starts and checks quietly. Nothing appears if there is no newer version.
2. If there is one, a dialog offers it with the notes and the download size is not shown -
   the whole file is downloaded before anything is replaced.
3. On acceptance the file downloads with a progress window, which can be cancelled. A
   cancelled or failed download leaves nothing behind.
4. The new executable is written beside the running one and checked against the published
   SHA-256, then handed to a PowerShell helper. The helper waits for Flowbit to exit, copies
   the file beside the target, verifies it again, moves it into place, and starts Flowbit
   again elevated.

## Why a helper script rather than Flowbit updating itself

Windows locks a running executable, so a process cannot overwrite its own image. A second
copy of Flowbit could not either, for the same reason. The helper has to be something else,
and PowerShell is present on every supported Windows and needs nothing shipped alongside.

The relaunch is elevated deliberately. An unelevated Flowbit cannot load its driver, so a
relaunch without `-Verb RunAs` would produce an update that appears to succeed and then
cannot shape any traffic.

## Checking it worked

In Flowbit: **Settings → Startup → Check now**. It reports one of:

- `Up to date (1.0.0).` - the check ran and found nothing newer
- `Version 1.1.0 is available.` - and offers it
- `Could not check: ...` - with the reason

The silent check at startup never shows a window. A network being unavailable is not worth
interrupting anyone over, so only a positive result is announced.
