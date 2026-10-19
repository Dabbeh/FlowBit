#pragma once

#include <QString>

namespace net {

// ---------------------------------------------------------------------------
// Built-in self-test  (Flowbit.exe --selftest)
// ---------------------------------------------------------------------------
//
// Purpose: let the owner of the installation confirm, at any time and without a
// developer present, that the parts of Flowbit that depend on the machine
// (driver, elevation, ETW monitoring, per-application attribution, per-app
// enforcement) are actually working.
//
// Design rules, learned from the debugging sessions that led to it:
//
//   * Every result is MEASURED, never assumed. Where a number can be read, it is
//     read and printed next to the verdict, so "PASS" always carries its own
//     evidence and can be argued with.
//   * A check that cannot run is reported as SKIP with the reason, never as a
//     failure and never as a pass. "Unknown" is a distinct outcome.
//   * Bounded in time. No step may block indefinitely: a wedged kernel object
//     must produce a FAIL with a reason, not a hung process. Consumers of trace
//     sessions that never exit are what turned a tooling bug into a phantom
//     "ghost session" that wasted hours, so nothing here may leak one.
//   * No side effects that outlive the run: the trace session is stopped and the
//     rule set is restored before returning.
//
// Returns a process exit code: 0 = every executed check passed, 1 = something
// failed, 2 = could not run at all (for example, not elevated).
int runSelfTest(const QString &reportPath);

} // namespace net
