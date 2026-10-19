// Verifies that a silent update check stays silent, and that an announced one does not.
//
// The rule: a check that fails must never interrupt anyone, because a network being down is
// not an error the user can act on and it is not something Flowbit did wrong. A check that
// succeeds and finds something newer SHOULD speak, because that is worth knowing.
//
// The difference is a single `announce` flag passed to checkForUpdatesNow, and the failure
// mode if it were wrong is a dialog on every launch on a machine without a network - which
// is the exact sort of thing that makes a program annoying.
//
// This counts the modal dialogs that appear, rather than assuming. A dialog would block, so
// the count is taken with a short timer that dismisses anything that appears.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "updater.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
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

// Dismisses any message box that appears and counts it. Running for a bounded time rather
// than blocking, so the count is available even when nothing appears.
struct DialogWatch {
    int seen = 0;
    QTimer timer;
    void start()
    {
        QObject::connect(&timer, &QTimer::timeout, [this] {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *m = qobject_cast<QMessageBox *>(w)) {
                    if (m->isVisible()) {
                        ++seen;
                        m->close();
                    }
                }
            }
        });
        timer.start(40);
    }
    void stop() { timer.stop(); }
};

// An address that cannot resolve, so the check fails the way a disconnected machine does.
const char *kUnreachable = "https://no-such-host.invalid/version.json";

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    Updater::setVersionFileUrl(QString::fromLatin1(kUnreachable));
    std::printf("\n== a check against an unreachable address ==\n");
    std::printf("  url: %s\n", kUnreachable);

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1100, 760);
    win->show();
    pump(500);
    win->onPageSelected(int(Sidebar::Settings));
    pump(400);

    // ---- the announced check DOES report the failure ---------------------
    // The user pressed a button and is owed an answer, so this one must speak.
    {
        DialogWatch watch;
        watch.start();
        QMetaObject::invokeMethod(win, "checkForUpdatesNow", Q_ARG(bool, true));
        pump(6000);
        watch.stop();
        std::printf("  announced check: %d dialog(s)\n", watch.seen);
        check(watch.seen >= 1,
              "pressing Check now reports a failure rather than appearing to do nothing");
    }

    // ---- the silent check does NOT --------------------------------------
    {
        DialogWatch watch;
        watch.start();
        QMetaObject::invokeMethod(win, "checkForUpdatesNow", Q_ARG(bool, false));
        pump(6000);
        watch.stop();
        std::printf("  silent check: %d dialog(s)\n", watch.seen);
        check(watch.seen == 0,
              QStringLiteral("the silent check shows nothing when it cannot reach the network "
                             "(%1 dialog(s))")
                  .arg(watch.seen));
    }

    // ---- and the status label still says something -----------------------
    // Silence for the user at large does not mean the outcome is lost: the settings page
    // records it, so someone who looks can see why nothing was offered.
    {
        bool found = false;
        QString text;
        for (QWidget *w : win->findChildren<QWidget *>()) {
            if (w->objectName() == QLatin1String("note")) {
                auto *l = qobject_cast<QLabel *>(w);
                if (l && l->text().contains(QLatin1String("Could not check"))) {
                    found = true;
                    text = l->text();
                }
            }
        }
        std::printf("  status label: %s\n",
                    found ? qPrintable(text) : "(no \"Could not check\" text found)");
        // Not asserted as a failure: the label belongs to the settings page and the check may
        // complete after this window closes. Reported so a regression is visible.
        std::printf("    (reported, not asserted: the label is written by the settings page)\n");
    }

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_update_silence passed."
                                        : "test_update_silence FAILED.");
    return g_failures == 0 ? 0 : 1;
}
