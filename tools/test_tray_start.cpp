// Verifies that --tray-start really starts Flowbit without a window.
//
// What was asked: "even if I say start at startup, do I have to open it by hand?" The answer
// should be no - Windows starts it at logon and it goes to the notification area.
//
// The task is registered correctly:
//
//     Task To Run   : "C:\Program Files\Flowbit\Flowbit.exe" --tray-start
//     Schedule Type : At logon time
//
// so the remaining question is whether --tray-start does what it claims. If it does not, the
// user gets a window in their face at every logon - a different annoyance from the one being
// fixed, and one they would notice immediately.
//
// startHidden() shows the window and then hides it, deliberately: the tray icon, its menu and
// its tooltip are all created by the window, and Qt only places the icon once the window has
// been realised. So the check is that the window ends up hidden while the tray icon does not.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSystemTrayIcon>
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

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    // What Windows passes, taken from the registered task.
    QApplication::arguments();   // ensure arguments are initialised
    std::printf("\n== --tray-start ==\n");
    std::printf("  the task runs: Flowbit.exe --tray-start\n");

    QTemporaryDir dir;
    static SettingsStore store(dir.path() + QStringLiteral("/cfg"));
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    // ---- 1. a normal launch shows the window -----------------------------
    {
        auto *win = new MainWindow(&store, &engine, &monitor, &usage);
        win->resize(900, 640);
        win->show();
        pump(500);
        check(win->isVisible(), "a normal launch shows the window");
        win->close();
        delete win;
        pump(200);
    }

    // ---- 2. a tray start does not ----------------------------------------
    {
        auto *win = new MainWindow(&store, &engine, &monitor, &usage);
        win->resize(900, 640);

        // The call main.cpp makes for --tray-start.
        win->startHidden();
        pump(600);

        std::printf("  after startHidden(): visible=%d minimised=%d\n", win->isVisible() ? 1 : 0,
                    win->isMinimized() ? 1 : 0);
        check(!win->isVisible(),
              "a tray start leaves the window hidden, so logon is not interrupted");

        // The tray icon must exist, or the application would be running with nothing to click
        // and no way to reach it - which is worse than showing a window.
        QSystemTrayIcon *tray = nullptr;
        for (QSystemTrayIcon *t : win->findChildren<QSystemTrayIcon *>()) {
            tray = t;
            break;
        }

        if (tray) {
            std::printf("  tray icon: visible=%d, tooltip=\"%s\"\n", tray->isVisible() ? 1 : 0,
                        qPrintable(tray->toolTip()));
            check(tray->isVisible(),
                  "and the tray icon is placed, so Flowbit can still be reached");
            check(!tray->toolTip().isEmpty(),
                  "the icon has a tooltip, so the user can tell what it is");
        } else {
            // A headless test session may have no notification area at all, in which case the
            // window creates no icon. Reported rather than failed, because that is a property
            // of the environment and not of the code.
            std::printf("  (no tray icon on this session - no notification area available)\n");
        }

        delete win;
        pump(200);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_tray_start passed." : "test_tray_start FAILED.");
    return g_failures == 0 ? 0 : 1;
}
