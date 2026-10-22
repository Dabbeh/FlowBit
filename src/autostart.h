#pragma once

#include <QString>

namespace net {

// Starting with Windows, and stopping doing so.
//
// TWO MECHANISMS, AND WHY BOTH EXIST
//
// Flowbit must run elevated to load the WinDivert driver, and a program cannot elevate itself
// without a prompt - that is what UAC is for. So which mechanism starts it decides whether the
// user sees a prompt at logon.
//
//   1. A Task Scheduler entry, run at logon with highest privileges. Windows itself launches
//      the process, already elevated, so NO prompt appears. This is the one to use.
//
//   2. The Run key, which is the classic approach and CANNOT elevate. On its own it would
//      either fail silently or raise a prompt at every logon - the exact annoyance being
//      complained about.
//
// The Run key is kept only as a fallback. A scheduled task can be removed by group policy on
// a managed machine, by a "cleanup" tool, or by someone tidying Task Scheduler; in that case a
// Run entry at least means Flowbit still starts. When both exist the task wins, because
// Windows runs it first and the second launch finds the single-instance guard already held.
namespace autostart {

// Path of the executable that would be registered, which is the running one.
QString executablePath();

// The command line stored for startup, quoted so a path containing spaces survives.
QString registeredCommand();

// The name the scheduled task is registered under.
QString taskName();

// Overrides the name used by BOTH mechanisms. Pass an empty string to restore "Flowbit".
//
// This exists so tools/test_autostart.cpp can drive the real code paths without touching the
// user's actual startup configuration. It was added because that test did exactly the
// opposite: run from a build directory its executable path differs from the installed one, so
// isEnabled() saw "a different copy", the test wrote its own Run value over the user's, and
// then deleted it on the way out - silently removing the user's startup entry.
//
// The application itself never calls this.
void setRegistrationName(const QString &name);

// True when startup is configured by EITHER mechanism, and points at this executable.
//
// Both are checked: reporting "off" while a Run entry existed would let the user turn the
// setting off and find Flowbit still starting.
bool isEnabled();

// Which mechanism is in use, for the settings page to report honestly. Empty when none is.
QString activeMechanism();

// Adds or removes both mechanisms. Returns false and fills `error` on failure.
//
// Enabling prefers the scheduled task, because that is the one that avoids the logon prompt,
// and falls back to the Run key when a task cannot be created - see the note above.
bool setEnabled(bool enabled, QString *error = nullptr);

// What the registry currently holds, for the diagnostics page. Empty when nothing is set.
QString registeredValue();

// True when a scheduled task exists, whatever its state.
bool taskExists();

// Creates or removes the scheduled task. Separate from setEnabled so the settings page can
// report which mechanism it managed to use.
bool createTask(QString *error = nullptr);
bool removeTask(QString *error = nullptr);

} // namespace autostart

} // namespace net
