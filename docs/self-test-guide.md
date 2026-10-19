# Flowbit self-test — how to run it and read the results

## What it is for

It checks the parts of Flowbit that depend on your PC rather than on the app's own
code: the packet driver, ETW monitoring, per-application attribution, and your
schedules. Run it whenever you want to know whether the app is actually working —
after a Windows update, after changing settings, or when something looks wrong.

## How to run it

1. **Start something that uses the network first** — a download, or a video. This
   matters: the monitoring check needs real traffic to measure, and with no traffic
   it will report `[SK]` (skipped) rather than a result.
2. Double-click **`Run-SelfTest.bat`** in the Flowbit folder.
3. Accept the **one UAC prompt**. (Monitoring needs Administrator rights; that is
   the same reason Flowbit itself asks.)
4. Read the window, or open **`selftest-report.txt`** afterwards — the same text is
   saved there so you can keep it or send it.

It takes about 25 seconds and changes nothing: the trace session it opens is
closed again before it exits.

### Running it without the .bat

From a Command Prompt in the Flowbit folder:

```
Flowbit.exe --selftest selftest-report.txt
```

The report path is optional; without it the result only appears in the window.

## How to read the result

Each line starts with a tag:

| Tag | Meaning |
|---|---|
| `[OK]` | Measured and working. The numbers next to it are the evidence. |
| `[!!]` | Failed. The text says what to do about it. |
| `[SK]` | Could not run — usually because nothing was using the network. Not a failure, but not proof either. |
| `[--]` | Information only. No verdict. |

The last line summarises: **`EVERY CHECK PASSED`**, or a count of failures.

## What each section means, and what to look for

### BASICS
The Windows build and whether the process is elevated. Every check below needs
elevation, so a failure here explains any later ones.

### ETW TRACE SESSION
Says whether a session named `Flowbit-AppMonitor` already exists. **This is
expected and fine while Flowbit is running.** If Flowbit is *not* running and this
still reports a session, it is a leftover from a killed run — the monitoring
section below then tells you whether it could still be used.

### PACKET DRIVER (per-application limiting)
That `WinDivert.dll` and `WinDivert64.sys` are next to the executable, and that the
driver actually opened a handle. **A failure here is the same fault the App Limiter
page shows as "the traffic driver could not be opened".** The line below it quotes
the driver's own reason, which is what makes it fixable.

### PER-APPLICATION MONITORING — the section that matters most

Three things are checked, and each carries its own numbers:

* **Kernel network events received** — how many events Windows delivered. Zero
  means either nothing used the network during the test, or this Windows build is
  not delivering them at all. This was a real problem on Windows build 26300 before
  the enable level was corrected.
* **Events decoded** — the percentage that produced a usable byte count. **Should
  be above 95%.** A low figure means Windows changed the event payload and the
  decoder needs updating; the report then includes the raw field list to make that
  possible.
* **Applications attributed during the test** — the actual measured rates. Read it
  like this:
  * The busiest application should be **whichever one you were actually using**.
  * While **downloading**, `down` must be the larger number.
  * While **uploading**, `up` must be the larger number.
  * **If those two are swapped, the direction mapping has regressed** — that exact
    bug happened before and is described in `RESOLVED-direction.md`.

### SCHEDULES
Shows whether system-wide limiting is switched on, which rule (if any) is active
right now, and lists your per-application rules with their schedule windows.

**The line to watch:** an entry that says `enabled` but shows a window like
`21:00-07:00` while nothing is being enforced means the window is not matching the
current time. Times are stored strictly as `HH:MM`; a window with unparseable text
never matches, which silently disables it.

## When to run it

| Situation | Why |
|---|---|
| Something looks wrong | Tells you which layer is at fault instead of guessing |
| After a Windows update | Kernel ETW behaviour is the thing most likely to change |
| Before reporting a problem | The report contains the measurements needed to diagnose it |
| After changing many settings | Confirms a schedule actually applies when you expect |

## What it does not check

* **Whether a bandwidth limit is respected.** That needs a real sustained transfer
  measured from outside the app; see the method in `RESOLVED-app-limiter.md`
  (configure a limit well below your line speed, download a fixed-size file, and
  time it — a working limit makes it take proportionally longer).
* Anything about the appearance or behaviour of the window itself.
