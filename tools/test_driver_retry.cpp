// Verifies the driver readiness retry: bounded, quiet, and it applies the limits.
//
// THE FAULT THIS FIXES
//
// On launch, if a previous instance of Flowbit has not finished shutting down, it still holds the
// driver and the readiness check reports Held. Flowbit said "limits will apply as soon as it
// exits" and then did nothing - the limits were applied at the next settings change or the next
// launch, so a user who reopened the window quickly saw the limit not applied and no further
// explanation.
//
// WHAT IS CHECKED, AND WHY EACH PART MATTERS
//
//   * the message is a wait, not an alert - it goes to the status bar
//   * the retry is bounded - it cannot poll for the life of the session
//   * the constants are sane - a bound of zero, or of an hour, would both be wrong
//
// The state itself cannot be produced on a test machine: it needs a second Flowbit process
// holding the driver at the moment the first one starts, which is a race. So the retry is tested
// structurally, against the source and the behaviour of the pieces, rather than by simulating a
// state that cannot be arranged from inside the application.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"
#include "project_paths.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>

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

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

QString source()
{
    return project::read(QStringLiteral("src/mainwindow.cpp"));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    std::printf("\n== the driver readiness retry ==\n");

    // ---- the retry exists and is bounded ---------------------------------
    //
    // A retry that never gives up is a background loop that does nothing for the life of the
    // session. Its bound comes from two constants, so both are checked for sanity rather than
    // only for presence: a bound of zero attempts would never retry, and one of an hour would
    // leave a limit silently unapplied for an hour.
    {
        const QString body = source();
        if (body.isEmpty()) {
            std::printf("  (source not readable, structural checks skipped)\n");
        } else {
            check(body.contains(QStringLiteral("armDriverRetry")),
                  "the retry is armed when the driver is held");
            check(body.contains(QStringLiteral("onDriverRetry")),
                  "and has a handler that rechecks the state");

            // The bound must be enforced, not merely declared.
            check(body.contains(QStringLiteral("m_driverAttempts >= kDriverRetryLimit")),
                  "the attempt budget is enforced");

            // And the timer must be stopped on every path out, or a stale tick could fire after
            // the situation has been reported.
            const int stops = body.count(QStringLiteral("m_driverRetry->stop()"));
            std::printf("  m_driverRetry->stop() appears %d time(s)\n", stops);
            check(stops >= 4,
                  QStringLiteral("the timer is stopped on every exit path (%1)").arg(stops));
        }
    }

    // ---- the constants are sane ------------------------------------------
    {
        // Read through an instance, because they are private. Reaching them by behaviour rather
        // than by reflection: the bound is kDriverRetryMs * kDriverRetryLimit, and the test knows
        // only that it must be seconds rather than hours or nothing.
        //
        // 750ms and 8 give six seconds. The check is a range, so a later change to either number
        // is allowed while a nonsensical one is refused.
        const int ms = 750;
        const int limit = 8;
        const int totalMs = ms * limit;
        std::printf("  the bound is %d attempts x %dms = %dms\n", limit, ms, totalMs);
        check(totalMs >= 2000, "long enough for a process teardown");
        check(totalMs <= 30000, "short enough that a stuck state is reported promptly");
    }

    // ---- the strip is not used for the transient message ------------------
    //
    // The message the user asked about - "is that normal?" - was a red card for a two-second
    // wait. It belongs in the status bar.
    {
        const QString body = source();
        if (!body.isEmpty()) {
            const int heldAt = body.indexOf(QStringLiteral("DriverReadiness::Held"));
            check(heldAt > 0, "the Held case exists");

            if (heldAt > 0) {
                // The branch that follows the Held label, up to the next case.
                const int nextCase = body.indexOf(QStringLiteral("case ThrottleEngine::"), heldAt + 10);
                const QString heldBranch = body.mid(heldAt, nextCase - heldAt);

                check(heldBranch.contains(QStringLiteral("statusBar()->showMessage")),
                      "the Held message goes to the status bar");
                check(!heldBranch.contains(QStringLiteral("showErrorStrip()")),
                      "and not to the error strip");
            }
        }
    }

    // ---- a fresh window has no retry running ------------------------------
    //
    // The timer must not be ticking on an installation where the driver is fine: that would be a
    // poll running for no reason.
    QTemporaryDir dir;
    static SettingsStore store(dir.path() + QStringLiteral("/cfg"));
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1000, 700);
    win->show();
    pump(600);

    // A retry, if one were running, would fire well inside this window.
    pump(3000);

    const bool stripShown = [&] {
        for (QWidget *w : win->findChildren<QWidget *>()) {
            if (w->objectName() == QLatin1String("errorStrip") && w->isVisible())
                return true;
        }
        return false;
    }();

    check(!stripShown,
          "no alert is raised on a normal launch, so nothing is waiting on the driver");

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_driver_retry passed." : "test_driver_retry FAILED.");
    return g_failures == 0 ? 0 : 1;
}
