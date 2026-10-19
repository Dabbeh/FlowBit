// Verifies which dialog the Schedule button opens, by application state.
//
// Reported twice from a screenshot: with no windows the manager appeared, showing an empty
// list, "No windows yet", and a single Add button - a dismissal between the user and the
// only action available. The first time a schedule is set it must go straight to the
// schedule dialog; the manager is only meaningful when there is something to manage.
//
// A dialog's appearance cannot be asserted without opening it, so this drives the real
// MainWindow and counts what the button leads to, using a helper that records which
// dialogs are shown rather than waiting for a user.

#include "mainwindow.h"
#include "appscheduleedialog.h"
#include "applimiterpage.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
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

// A rule with a window already on it, or without one.
AppRule makeRule(quint64 id, bool withWindow)
{
    AppRule r;
    r.id = id;
    r.name = QStringLiteral("Mozilla Firefox");
    r.exe = QStringLiteral("firefox.exe");
    r.path = QStringLiteral("C:/Program Files/Mozilla Firefox/firefox.exe");
    r.down = 256 * 1024;
    r.up = 666 * 1024;
    r.enabled = true;
    if (withWindow) {
        Rule w;
        w.id = 9911;
        w.name = r.name;
        w.from = QStringLiteral("22:00");
        w.to = QStringLiteral("07:00");
        w.days = 0x7F;
        w.down = 256 * 1024;
        w.up = 666 * 1024;
        r.windows << w;
    }
    return r;
}

// Opens the app limiter page and clicks Schedule on the first rule, reporting which
// dialog class came up. The dialog is closed immediately, so nothing is saved.
QString whichDialogOpens(MainWindow *win)
{
    QString seen;

    // Watch for any dialog being shown, and close it. A dialog with a zero-timer close is
    // the only way to observe one without a user.
    const auto watcher = [&seen] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            auto *d = qobject_cast<QDialog *>(w);
            if (!d || !d->isVisible() || !d->isModal())
                continue;
            if (qobject_cast<AppScheduleDialog *>(d))
                seen = QStringLiteral("AppScheduleDialog");
            else
                seen = QString::fromLatin1(d->metaObject()->className());
            d->reject();   // close without accepting, so nothing is written
        }
    };

    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, watcher);
    poll.start(20);

    if (auto *side = win->findChild<Sidebar *>())
        win->onPageSelected(int(Sidebar::AppLimiter));
    pump(250);

    auto *page = win->findChild<AppLimiterPage *>();
    if (!page) {
        poll.stop();
        return QStringLiteral("(no app limiter page)");
    }

    // The Schedule button on the first rule card.
    QPushButton *schedule = nullptr;
    for (QPushButton *b : page->findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Schedule")) {
            schedule = b;
            break;
        }
    }
    if (!schedule) {
        poll.stop();
        return QStringLiteral("(no Schedule button)");
    }

    schedule->click();
    pump(400);
    poll.stop();
    return seen;
}

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

    std::printf("\n== which dialog does Schedule open? ==\n");

    // ---- with no windows: the schedule dialog directly --------------------
    {
        Settings s = store.load();
        s.appRules.clear();
        s.appRules << makeRule(5001, false);
        s.rules.clear();
        QString err;
        store.save(s, &err);

        auto *win = new MainWindow(&store, &engine, &monitor, &usage);
        win->resize(1200, 800);
        win->show();
        pump(400);

        const QString which = whichDialogOpens(win);
        std::printf("  no windows -> %s\n", qPrintable(which));
        check(which != QLatin1String("AppScheduleDialog"),
              "with no windows the manager is SKIPPED");
        check(which.contains(QLatin1String("ScheduleDialog")),
              QStringLiteral("with no windows the schedule dialog opens (%1)").arg(which));

        win->close();
        delete win;
        pump(150);
    }

    // ---- with a window: the manager, so it can be edited -------------------
    {
        Settings s = store.load();
        s.appRules.clear();
        s.appRules << makeRule(5002, true);
        s.rules.clear();
        QString err;
        store.save(s, &err);

        auto *win = new MainWindow(&store, &engine, &monitor, &usage);
        win->resize(1200, 800);
        win->show();
        pump(400);

        const QString which = whichDialogOpens(win);
        std::printf("  one window -> %s\n", qPrintable(which));
        check(which == QLatin1String("AppScheduleDialog"),
              QStringLiteral("with a window the manager opens, so it can be edited (%1)")
                  .arg(which));

        win->close();
        delete win;
        pump(150);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_schedule_routing passed."
                                        : "test_schedule_routing FAILED.");
    return g_failures == 0 ? 0 : 1;
}
