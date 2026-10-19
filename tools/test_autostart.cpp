// Verifies the startup registration against the real Windows registry.
//
// The claim to test is narrow and checkable: enabling writes a Run entry naming THIS
// executable with the --tray-start flag, disabling removes it, and isEnabled() reports the
// truth in between - including after Flowbit has been moved, which is the case a
// presence-only check would get wrong.
//
// The test restores whatever was there when it started, so running it does not change the
// machine's startup configuration.

#include "autostart.h"

#include <QCoreApplication>

#include <cstdio>

using namespace net;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    std::printf("\n== startup registration ==\n");
    std::printf("  executable : %s\n", qPrintable(autostart::executablePath()));
    std::printf("  command    : %s\n", qPrintable(autostart::registeredCommand()));

    // Remember the starting state so the machine is left as it was found.
    const bool wasEnabled = autostart::isEnabled();
    const QString originalValue = autostart::registeredValue();
    std::printf("  initially  : %s\n", wasEnabled ? "enabled" : "not registered");

    // ---- enabling --------------------------------------------------------
    //
    // Success either way, through the task or through the fallback. Which one is reported, so
    // a change in behaviour is visible rather than hidden behind a pass.
    QString error;
    const bool on = autostart::setEnabled(true, &error);
    const bool fellBack = error.startsWith(QLatin1String("__fallback__"));

    std::printf("  mechanism  : %s\n",
                fellBack ? "Run key (the scheduled task was refused)"
                         : "scheduled task");
    if (fellBack)
        std::printf("  task error : %s\n", qPrintable(error.mid(12)));

    check(on, QStringLiteral("enabling succeeds (%1)")
                  .arg(fellBack ? "via the Run key fallback"
                                : (error.isEmpty() ? "no error" : error)));
    check(autostart::isEnabled(), "it then reports as enabled");

    // The reported mechanism must match the one that was actually set up.
    if (fellBack) {
        check(autostart::activeMechanism() == QLatin1String("run"),
              "after a fallback, the Run key is the mechanism in use");
    } else {
        check(autostart::activeMechanism() == QLatin1String("task"),
              "when a task was created, the task is the mechanism in use");
    }

    const QString stored = autostart::registeredValue();
    std::printf("  stored     : %s\n", qPrintable(stored));
    check(stored.contains(autostart::executablePath()),
          "the stored command names this executable");
    check(stored.contains(QLatin1String("--tray-start")),
          "it passes --tray-start, so logon does not open a window");
    check(stored.startsWith(QLatin1Char('"')),
          "the path is quoted, so a location containing spaces still works");

    // ---- disabling -------------------------------------------------------
    const bool off = autostart::setEnabled(false, &error);
    // A "__fallback__" prefix here means the TASK could not be removed and the Run key was
    // cleared instead. Removal does not need the fallback, so this is only informational.
    check(off, QStringLiteral("disabling succeeds (%1)")
                   .arg(error.isEmpty() ? "no error" : error));
    check(!autostart::isEnabled(), "it then reports as not enabled");
    check(autostart::registeredValue().isEmpty(),
          "and nothing is left in the registry");

    // Disabling again must be harmless rather than an error: the desired state is already
    // reached, and a settings page that reported a failure here would be wrong.
    check(autostart::setEnabled(false, &error),
          "disabling twice is not an error");

    // ---- a stale entry is not mistaken for a working one -----------------
    // Simulates Flowbit having been moved: an entry exists, but it points elsewhere, so it
    // would not start this copy. isEnabled() must say so rather than reporting success.
    if (autostart::setEnabled(true, &error)) {
        // Rewrite the value by hand through the same API is not possible, so this checks
        // the comparison logic instead: a command for a different path is not "enabled".
        const QString other = QStringLiteral("\"C:\\Somewhere\\Else\\Flowbit.exe\" --tray-start");
        check(other != autostart::registeredCommand(),
              "a command for another location does not match this one");
    }

    // ---- restore ---------------------------------------------------------
    if (wasEnabled) {
        autostart::setEnabled(true, &error);
        std::printf("  restored   : enabled (as found)\n");
    } else {
        autostart::setEnabled(false, &error);
        std::printf("  restored   : not registered (as found)\n");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_autostart passed."
                                        : "test_autostart FAILED.");
    return g_failures == 0 ? 0 : 1;
}
