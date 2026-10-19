# Plan: a universal reset

## What a reset has to cover

The reset must return Flowbit to the state of a fresh install. Everything the user can change
falls into four groups, and they live in different places, which is why a reset is not simply
"delete the settings file".

### 1. The settings file

`%LOCALAPPDATA%\Flowbit\settings.json` holds:

| Field | Meaning |
|---|---|
| `enabled` | the master switch |
| `defaultDown`, `defaultUp` | the baseline limits |
| `rules` | the system-wide schedules, each with its own id |
| `appRules` | the per-application limits, each with its own schedules |
| `usage` | the 30-day quota configuration |
| `showThroughput` | the dashboard readout |
| `themeMode` | appearance |
| `startWithWindows`, `closeToTray`, `checkForUpdates` | the startup and closing behaviour |
| `skippedVersion` | an update the user dismissed |

### 2. Usage history

`usage-YYYY-MM.json` and `apps-YYYY-MM.json`, one pair per month. This is measured data rather
than a preference, and it is the one part a user might want to keep: deleting it loses the
30-day figures and the per-application breakdown, which cannot be recovered.

**So a reset should offer the choice rather than deciding.** Two options:

- **Reset settings** - preferences, limits and schedules. Keeps the measured history.
- **Reset everything** - also deletes the usage history.

### 3. Things outside the settings file

These are not in `settings.json` and would survive a naive reset, which is the trap:

| Item | Where | Reset must |
|---|---|---|
| Start with Windows | a scheduled task, or the Run key | remove both |
| Window placement | Qt's own `QSettings` (the registry, under the organisation name) | delete the key |
| The generated tick | `checkmark.svg` in the data folder | can be left; regenerated |
| The driver service | a kernel service | left alone; it is not a user preference |
| Migrated NetValve data | `%LOCALAPPDATA%\NetValve` | left alone; it is the source of a migration, not a setting |

The scheduled task is the important one. Without removing it, "reset" would leave Flowbit set
to start at logon while the settings page shows the box unticked - the exact disagreement
between the file and the mechanism that the startup work went to some trouble to avoid.

### 4. The running state

After resetting, the application must apply the defaults immediately rather than at the next
launch: the engine reconfigured, the pages rebuilt, the theme reapplied, the tray tooltip
refreshed. A reset that only took effect after a restart would look broken.

## The design

A **Danger** card at the bottom of the Settings page, below Diagnostics where the rare and
destructive actions belong.

```
┌─ RESET ─────────────────────────────────────────────────┐
│ Reset all settings                                       │
│ Returns Flowbit to a fresh install: limits, schedules,   │
│ appearance and startup behaviour. Your usage history is   │
│ kept.                                                    │
│                                        [ Reset settings ] │
│                                                          │
│ Reset everything including usage history                 │
│ Also deletes the 30-day data usage figures and the        │
│ per-application breakdown. This cannot be undone.        │
│                                     [ Reset everything ]  │
└──────────────────────────────────────────────────────────┘
```

**Confirmation before acting.** A destructive action that happens on one click is a mistake
waiting to be made. The dialog states exactly what will be removed and, for the second option,
that the history cannot be recovered.

**No undo, so the wording has to be exact.** The confirmation names the counts - "3 schedules
and 5 application rules" - so the user knows what they are about to lose.

## What must not be touched

- **The driver service.** It is installed state, not a preference. Removing it would leave
  Flowbit unable to shape traffic until it re-registered the service, which needs the driver
  file. A reset is about the user's choices.
- **The NetValve folder.** It is the source of a one-time migration. Deleting it would remove
  data the user never asked Flowbit to manage, and the migration would look like a fresh
  install on the next launch.
- **Installed files.** A reset is not an uninstall. The application, its libraries and its
  driver stay.

## Verification

- The settings file is recreated with defaults.
- Schedules and application rules are gone; the pages show their empty states.
- The startup task and Run key are both removed, and the Settings checkbox is unticked.
- The usage history survives option 1 and is gone after option 2.
- The engine stops limiting, because the default has `enabled = false`.
- Qt's window-placement key is cleared, so the window opens at its default size.

## Order

1. A `resetToDefaults(bool includeHistory)` method on the store, because it owns the paths.
2. The card and its two buttons.
3. The confirmation, naming what will be lost.
4. Applying the result to the running application.
5. A test that performs each reset against a temporary directory and checks every file and
   registry entry afterwards.
