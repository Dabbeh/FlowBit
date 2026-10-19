# Plan: finish the remaining work

Written after inspecting the current state rather than from memory. Each phase states what
is already done, what is missing, and how the result will be checked.

---

## Phase 1 — The driver starts automatically (in progress)

**Problem.** `START_TYPE: 4 DISABLED` with `STATE: 4 RUNNING`. The driver is loaded now but
unstartable after a reboot, so a first launch can report a failure that a retry would not.
Full analysis in `docs/windivert-service-disabled.md`.

**Already done.** `ThrottleEngine` detects the state, explains it, repairs the start type,
verifies the registered path belongs to us before doing so, and retries the open once.

**Missing.** The repair happens *on demand* — after `WinDivertOpen` has already failed. So
the first attempt is the one the user sees fail.

**What to build.**

1. A `ensureDriverReady()` that runs at startup, before the first apply:
   - read the start type;
   - if DISABLED **and** the path is ours, repair it and start the service;
   - if DISABLED but another instance holds the driver, say so rather than retrying blindly;
   - if MARKED_FOR_DELETE, report that a reboot is required, because no command clears it.
2. Run it off the UI thread, so a slow driver load cannot freeze the window.
3. Show the outcome in Diagnostics as a state, not only as an error when something fails.

**Checked by.** A test that sets the start type to 4, calls `ensureDriverReady()`, and
asserts the start type is back to 3 and the open succeeds — plus the negative case, that a
service pointing at a different path is left alone.

**Not in scope.** Installing to a stable location is a packaging fix and is already done by
the installer. Running from the Desktop will keep producing this state; that is documented
rather than worked around.

---

## Phase 2 — The Settings UI (the main remaining work)

**Problem.** Three features are built, tested and committed, and none of them can be reached.
`startWithWindows` defaults to off and `closeToTray` to the current behaviour, so from the
user's side nothing has changed.

**What to build**, in the existing Settings page, in the existing visual style:

| Control | Setting |
|---|---|
| Start Flowbit when Windows starts | `startWithWindows` |
| When I close the window: minimise to tray / quit | `closeToTray` |
| Check for updates automatically | `checkForUpdates` |
| Check for updates now (button) | — |

Plus the conflict guard, already agreed: if the tray icon is unavailable or hidden and
`closeToTray` is on, the combination is refused with an explanation, because Flowbit would
be running with no way to reach it.

**Checked by.** A test that toggles each control, saves, reloads the settings file and
confirms the value round-tripped — and that the registry matches for the startup entry.

---

## Phase 3 — The update channel

**Problem.** The updater is finished but has nothing to read. `docs/version.json` does not
exist.

**What to build.**

1. `docs/version.json` in the repository:
   ```json
   {
     "version": "1.0.0",
     "url": "https://github.com/Dabbeh/FlowBit/releases/download/v1.1.0/Flowbit.exe",
     "notes": "What changed in this release."
   }
   ```
   Editing this one file is how a release is published.
2. Wire `checkForUpdates` into startup: a silent check, and a dialog **only** when a newer
   version exists. A failed check stays quiet — the network being down is not an error worth
   interrupting anyone over.
3. The "Check for updates now" button reports all three outcomes, including "up to date",
   because the user asked explicitly.

**Checked by.** Pointing the updater at a local file with a higher version and confirming it
is offered; then at the real URL and confirming it reports up to date against version 1.0.0.

---

## Phase 4 — Ship it

1. Rebuild the installer with the new sources.
2. Verify the installed application starts, finds its plugins, and shapes traffic.
3. Confirm the uninstaller removes what it installed and leaves settings behind.
4. Commit and push.

---

## Phase 5 — Close the known gaps

- **`run_diag_tests`** still has 2 stale assertions encoding behaviour that has since
  changed. They have been failing since before this plan and should be corrected or removed.
- **Deploy the delete fix** to the Desktop copy. Blocked while that copy is running
  elevated; the installer is the clean route.
- Remove the leftover `flowbit-self-update.ps1` if an update run leaves one behind.

---

## Order and reasoning

Phases 1 and 2 are independent; 3 depends on nothing but is the least useful until 2 exists,
since the update button lives in the settings page. Phase 4 must come last.

Phase 1 goes first because it is the problem reported most recently and it is the one that
makes the application look broken on a fresh start.
