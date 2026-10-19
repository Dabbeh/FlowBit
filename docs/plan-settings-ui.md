# Plan: finish the Settings UI, and fix what the probe found

## What the probe found

```
== the settings page's chain ==
  scroll area      : 68x0   visible=0
  its viewport     : 58x0
  its content      : 325x684

  the stack shows index 4 of 5
    page 0: 1083x791  visible=0
    page 1: 100x30    visible=0
    page 2: 100x30    visible=0
    page 3: 100x30    visible=0   <- holds the scroll area
    page 4: 1083x791  visible=1
```

**The stack is showing index 4, and the Settings page is index 3.**

`emit side->pageSelected(...)` does not switch the page. The signal is an *output* of the
sidebar - it says the user clicked - and the window connects it to `onPageSelected`, which
does the switching. Emitting it from outside therefore does nothing except call whatever is
connected; the stack stays where it was.

That is a **fault in the test harness, not in the application.** It also explains every
measurement that looked wrong:

| Reported | Actually means |
|---|---|
| `viewport 0px` | The page was never shown, so it has no geometry. |
| `reaches 684px` while the card sits at 1428px | The 684 is the *content* height of a page that was never laid out at the window's width. |
| The screenshot never scrolled | The window was still on the Dashboard; `grab()` captured that. |
| The card measured 30x100 at first | A real bug, now fixed by `layout->addWidget(card)`. |

So most of the noise in this session was the harness measuring a page it had not opened.
The application-side bug - the card being built but never added to the layout - was real and
is fixed.

## What is already done and verified

| Item | Evidence |
|---|---|
| Startup card exists with the right controls | 2 checkboxes, 1 combo, 1 button |
| It is laid out | 1041x167px |
| It reads the saved settings | close, startup and update choices all match the file |
| The note for a refusal exists | found by object name |
| Phase 1 driver readiness | `ensureDriverReady()` built; detects the DISABLED state on this machine |

One test failure remains and it is the harness bug above.

---

## Phase A — Fix the harness, then re-verify (immediate)

**A1.** Change the probe and the two card tests to switch pages the way the application
does: call the window's page handler rather than emitting the sidebar's signal. The sidebar
should be used to *find the index*, not to perform the switch.

**A2.** Re-run and confirm the page reports a real viewport, and that the card is inside the
scrollable extent.

**A3.** Take one screenshot with the page genuinely current. This is the check that has
caught things twice in this project, and it has not yet been obtained for this card.

**A4.** Remove the misleading assertion. A test that cannot fail meaningfully is worse than
no test: assert the card is reachable *after* the page is shown, and if the harness cannot
show it, assert what can be established instead.

---

## Phase B — Finish the Settings UI (the remaining work)

**B1.** The conflict guard, properly. The rule agreed earlier: if the tray icon is
unavailable and X is set to hide, Flowbit would run unreachable. Currently this is only
*noted*. It should also **disable the "Minimise" option** in the combo, so the impossible
combination cannot be chosen rather than being chosen and then explained.

**B2.** The startup toggle must survive being moved. `isEnabled()` compares the full command,
so an entry pointing at an old location reads as "not registered". The note says so; the fix
is to rewrite the entry whenever the toggle is switched on, which `applyStartupSetting()`
already does - verify it end to end, including a deliberately stale entry.

**B3.** Making the update controls report honestly. `checkForUpdatesNow(true)` needs a
visible result for all three outcomes. The status label exists; verify each path.

**B4.** A test for the settings round trip: toggle each control, save, reload, confirm.

---

## Phase C — The update channel

**C1.** Create `docs/version.json` with version 1.0.0 and a placeholder URL, so the checker
has a real file to read and reports "up to date" correctly.

**C2.** Point the updater at a local file in a test and confirm a higher version is offered
and a lower one is not.

**C3.** Confirm a silent check at startup reports nothing when the network is unavailable.

---

## Phase D — Ship

**D1.** Rebuild the installer with the new sources.
**D2.** Re-run the full test suite and record which suites pass.
**D3.** Commit and push with the plan and the diagnosis documents.

---

## Phase E — Clear the known debts

**E1.** `run_diag_tests` has 2 stale assertions. Read them, decide whether the code or the
test is wrong, and fix whichever it is.
**E2.** Deploy to the Desktop copy so the delete fix and the Settings UI are both live.
**E3.** Delete the leftover `flowbit-self-update.ps1` handling if an update run leaves one.

---

## The rule this session keeps re-learning

Three times now a check has been declared passing on the strength of a measurement of
something that was not actually being exercised:

- the dialog tested by finding the *first* combo box, which was a speed-unit selector;
- the card measured while its page was never opened;
- the screenshot believed to show a page the window had not switched to.

Each time the code compiled and the numbers looked plausible. What caught it was looking at
the artefact itself - the screenshot - or printing the surrounding structure rather than the
one value under test. So: **when a measurement disagrees with an expectation, print the
chain around it before adjusting either.** That is what produced the finding above, after
two attempts spent adjusting the assertion instead.
