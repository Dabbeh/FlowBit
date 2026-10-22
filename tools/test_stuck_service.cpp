// Verifies that the stuck-service repair runs for every user, with no action from them.
//
// THE TWO WAYS THE PREVIOUS ATTEMPTS FAILED
//
// 1. The repair lived in the packet worker, which only starts when a limit is set. A fresh
//    install has limiting off, so it never ran and the user saw the same message on every launch
//    with nothing able to clear it.
//
// 2. It returned NeedsRestart without attempting the stop, so a state that is clearable was
//    reported as requiring a reboot.
//
// Both are structural faults, and both are checkable against the source and against a live
// window. The state itself cannot be created on a test machine - it needs a service marked for
// deletion while its driver is loaded - so the checks are of the paths rather than of a
// reproduced state, which is stated plainly rather than dressed up.

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
#include <QTemporaryDir>
#include <QThread>
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

QString read(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    std::printf("\n== the stuck-service repair, and whether users get it ===\n");

    const QString engine =
        read(project::file(QStringLiteral("src/throttleengine.cpp")));
    check(!engine.isEmpty(), "the engine source is readable");

    if (!engine.isEmpty()) {
        // ---- 1. it runs from the startup check, not the worker -------------
        //
        // The whole reason the first attempt fixed nothing. ensureDriverReady is called on every
        // launch from persistAndApply; the worker only starts when a rate is set.
        const int ensureAt = engine.indexOf(QStringLiteral("ThrottleEngine::ensureDriverReady()"));
        const int repairCallInEnsure =
            engine.indexOf(QStringLiteral("repairDisabledDriverService()"), ensureAt);
        const int workerAt = engine.indexOf(QStringLiteral("void ThrottleEngine::workerMain"));

        std::printf("  ensureDriverReady at %d, workerMain at %d\n", ensureAt, workerAt);

        check(ensureAt > 0 && repairCallInEnsure > ensureAt && repairCallInEnsure < workerAt,
              "the repair is called from ensureDriverReady, which runs on every launch");
        check(!(repairCallInEnsure > workerAt),
              "and not only from the packet worker, which needs a limit to be set");
    }

    // ---- 2. it actually attempts the unload ------------------------------
    //
    // Returning NeedsRestart without trying the stop is what made a clearable state look like it
    // needed a reboot. The stop is the specific thing that clears it.
    if (!engine.isEmpty()) {
        check(engine.contains(QStringLiteral("SERVICE_CONTROL_STOP")),
              "the repair attempts to stop the service, which is what completes the deletion");
        check(engine.contains(QStringLiteral("SERVICE_STOP | SERVICE_QUERY_STATUS")),
              "and opens the service with the rights the stop needs");

        // ---- 3. it verifies the outcome rather than assuming --------------
        //
        // NeedsRestart must be the result of checking, not of giving up when a call failed.
        check(engine.contains(QStringLiteral("const DriverServiceState after = readDriverServiceState();")),
              "the state is read back after the attempts, so the outcome is measured");
        check(engine.contains(QStringLiteral("if (!after.exists")),
              "a service whose deletion completed counts as repaired");
        check(engine.contains(QStringLiteral("if (after.startType != 4")),
              "as does one that is startable again");
    }

    // ---- 4. the uninstaller no longer creates the state ------------------
    //
    // Clearing it helps machines that have it; not creating it matters more.
    {
        const QString iss = read(project::file(QStringLiteral("installer/flowbit.iss")));
        check(!iss.isEmpty(), "the installer script is readable");
        if (!iss.isEmpty()) {
            check(!iss.contains(QStringLiteral("Parameters: \"delete WinDivert\"")),
                  "the uninstaller no longer runs sc delete straight after sc stop");
            check(iss.contains(QStringLiteral("flowbit-remove-driver.ps1")),
                  "it runs the script that waits for the driver to unload first");

            const QString script =
                read(project::file(QStringLiteral("installer/flowbit-remove-driver.ps1")));
            check(!script.isEmpty(), "and that script is present");
            check(script.contains(QStringLiteral("'STOPPED'")),
                  "the script polls for the stopped state rather than sleeping and hoping");
        }
    }

    // ---- 5. a normal launch raises no alert -------------------------------
    //
    // The repair runs on every launch, so it must be quiet when there is nothing wrong. A
    // startup that always showed something would be its own fault.
    QTemporaryDir dir;
    static SettingsStore store(dir.path() + QStringLiteral("/cfg"));
    static ThrottleEngine engineObj;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engineObj, &monitor, &usage);
    win->resize(1000, 700);
    win->show();
    pump(700);
    win->onPageSelected(int(Sidebar::Dashboard));
    pump(2500);

    const bool alert = [&] {
        for (QWidget *w : win->findChildren<QWidget *>()) {
            if (w->objectName() == QLatin1String("errorStrip") && w->isVisible())
                return true;
        }
        return false;
    }();

    check(!alert, "a normal launch shows no alert while the driver is healthy");

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_stuck_service passed."
                                        : "test_stuck_service FAILED.");
    return g_failures == 0 ? 0 : 1;
}
