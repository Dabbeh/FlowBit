// Verifies the Schedule button's dialog behaves as asked.
//
// What was asked for, plainly: ONE dialog. Clicking Schedule opens it showing the time
// already set, so it can be changed; the first time it opens blank; an "Add" button
// appears only when a time already exists.
//
// This drives the real MainWindow and the real dialog, checking:
//   * what the dialog is titled (Add Schedule vs Edit Schedule)
//   * whether the times shown are the stored ones or a blank default
//   * whether the Add button exists, in each case

#include "mainwindow.h"
#include "applimiterpage.h"
#include "scheduledialog.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
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

struct Observation {
    bool opened = false;
    QString title;
    QString from;
    QString to;
    QString saveLabel;
    bool hasAdd = false;
};

AppRule makeRule(quint64 id, bool withWindow)
{
    AppRule r;
    r.id = id;
    r.name = QStringLiteral("Mozilla Firefox");
    r.exe = QStringLiteral("firefox.exe");
    r.down = 256 * 1024;
    r.up = 666 * 1024;
    r.enabled = true;
    if (withWindow) {
        Rule w;
        w.id = 9911;
        w.name = r.name;
        // Deliberately NOT the default the dialog uses, so seeing these values proves the
        // window was loaded rather than the defaults being re-created.
        w.from = QStringLiteral("02:00");
        w.to = QStringLiteral("03:00");
        w.days = 0x7F;
        w.down = 256 * 1024;
        w.up = 666 * 1024;
        r.windows << w;
    }
    return r;
}

// Clicks Schedule and inspects the dialog that appears, then closes it.
Observation observe(MainWindow *win)
{
    Observation out;

    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&out] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            auto *d = qobject_cast<ScheduleDialog *>(w);
            if (!d || !d->isVisible() || out.opened)
                continue;
            out.opened = true;
            out.title = d->windowTitle();
            // The time fields are the only QLineEdits holding a clock value here.
            for (QLineEdit *e : d->findChildren<QLineEdit *>()) {
                const QString text = e->text();
                if (text.contains(QLatin1Char(':')) && out.from.isEmpty())
                    out.from = text;
                else if (text.contains(QLatin1Char(':')) && out.to.isEmpty())
                    out.to = text;
            }
            for (QPushButton *b : d->findChildren<QPushButton *>()) {
                if (b->text() == QLatin1String("Add"))
                    out.hasAdd = true;
                if (b->text().startsWith(QLatin1String("Save")))
                    out.saveLabel = b->text();
            }
            d->reject();   // close without saving
        }
    });
    poll.start(15);

    if (auto *side = win->findChild<Sidebar *>())
        win->onPageSelected(int(Sidebar::AppLimiter));
    pump(250);

    auto *page = win->findChild<AppLimiterPage *>();
    if (!page) {
        poll.stop();
        return out;
    }
    QPushButton *schedule = nullptr;
    for (QPushButton *b : page->findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Schedule")) {
            schedule = b;
            break;
        }
    }
    if (!schedule) {
        poll.stop();
        return out;
    }

    schedule->click();
    pump(400);
    poll.stop();
    return out;
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

    // ---- nothing set: blank, no Add --------------------------------------
    {
        Settings s = store.load();
        s.appRules.clear();
        s.appRules << makeRule(6001, false);
        s.rules.clear();
        QString err;
        store.save(s, &err);

        auto *win = new MainWindow(&store, &engine, &monitor, &usage);
        win->resize(1200, 800);
        win->show();
        pump(400);

        const Observation o = observe(win);
        std::printf("\n== no time set yet ==\n");
        std::printf("  title \"%s\"  from \"%s\"  to \"%s\"  save \"%s\"  add=%s\n",
                    qPrintable(o.title), qPrintable(o.from), qPrintable(o.to),
                    qPrintable(o.saveLabel), o.hasAdd ? "yes" : "no");
        check(o.opened, "the schedule dialog opens");
        check(o.title.contains(QLatin1String("Add")),
              QStringLiteral("it is the Add form (%1)").arg(o.title));
        check(!o.hasAdd, "no Add button when nothing is set yet");

        win->close();
        delete win;
        pump(150);
    }

    // ---- a time is set: shown, with Add ----------------------------------
    {
        Settings s = store.load();
        s.appRules.clear();
        s.appRules << makeRule(6002, true);
        s.rules.clear();
        QString err;
        store.save(s, &err);

        auto *win = new MainWindow(&store, &engine, &monitor, &usage);
        win->resize(1200, 800);
        win->show();
        pump(400);

        const Observation o = observe(win);
        std::printf("\n== a window is already set ==\n");
        std::printf("  title \"%s\"  from \"%s\"  to \"%s\"  save \"%s\"  add=%s\n",
                    qPrintable(o.title), qPrintable(o.from), qPrintable(o.to),
                    qPrintable(o.saveLabel), o.hasAdd ? "yes" : "no");
        check(o.opened, "the schedule dialog opens");
        check(o.title.contains(QLatin1String("Edit")),
              QStringLiteral("it is the Edit form (%1)").arg(o.title));
        check(o.from == QLatin1String("02:00"),
              QStringLiteral("the stored start time is shown (%1)").arg(o.from));
        check(o.to == QLatin1String("03:00"),
              QStringLiteral("the stored end time is shown (%1)").arg(o.to));
        check(o.hasAdd, "an Add button is offered when a time exists");
        check(o.saveLabel.contains(QLatin1String("Changes")),
              QStringLiteral("saving is worded as an edit (%1)").arg(o.saveLabel));

        win->close();
        delete win;
        pump(150);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_schedule_simple passed."
                                        : "test_schedule_simple FAILED.");
    return g_failures == 0 ? 0 : 1;
}
