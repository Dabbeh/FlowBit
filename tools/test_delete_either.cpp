// Verifies that deleting EITHER window works, driven through the real MainWindow.
//
// Reported: with two schedules, the second could be deleted but the first could not. The
// cause was in the caller, not the dialog - removedIds() was applied and then result() was
// written back, and result() is only set by accept(), so it still named the window that had
// just been removed and the "replace or append" step put it straight back.
//
// A dialog-only test could not catch that, because the dialog was correct: it reported the
// deletion properly. The fault was in how the caller applied the report, so this test drives
// the caller - builds a real MainWindow, opens the Schedule button's dialog, deletes the
// window on screen, and checks what was actually saved.

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
#include <QComboBox>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPushButton>
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

Rule makeWindow(quint64 id, const QString &from, const QString &to)
{
    Rule r;
    r.id = id;
    r.name = QStringLiteral("Firefox");
    r.from = from;
    r.to = to;
    r.days = 0x7F;
    r.down = 1024 * 1024;
    r.up = 512 * 1024;
    r.enabled = true;
    return r;
}

// Answers any confirmation box that appears, so the delete can proceed unattended.
void autoConfirm(QTimer &poll)
{
    QObject::connect(&poll, &QTimer::timeout, [] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *m = qobject_cast<QMessageBox *>(w)) {
                if (!m->isVisible())
                    continue;
                if (QAbstractButton *yes = m->button(QMessageBox::Yes))
                    yes->click();
                else if (QAbstractButton *discard = m->button(QMessageBox::Discard))
                    discard->click();
            }
        }
    });
}

// Opens Schedule on the first app rule, deletes whichever window is shown, saves, and
// reports what the settings store now holds.
QVector<Rule> deleteShownWindow(SettingsStore &store, int whichToSelect)
{
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1200, 800);
    win->show();
    pump(400);

    if (auto *side = win->findChild<Sidebar *>())
        win->onPageSelected(int(Sidebar::AppLimiter));
    pump(300);

    QTimer poll;
    autoConfirm(poll);
    poll.start(25);

    auto *page = win->findChild<AppLimiterPage *>();
    std::printf("    app limiter page found: %s\\n", page ? "yes" : "NO");
    QPushButton *schedule = nullptr;
    if (page) {
        QStringList labels;
        for (QPushButton *b : page->findChildren<QPushButton *>())
            labels << b->text();
        std::printf("    buttons on the page: %s\\n",
                    qPrintable(labels.join(QStringLiteral(" | "))));
        for (QPushButton *b : page->findChildren<QPushButton *>()) {
            if (b->text() == QLatin1String("Schedule")) {
                schedule = b;
                break;
            }
        }
    }
    std::printf("    Schedule button: %s\\n", schedule ? "found" : "NOT FOUND");
    if (schedule)
        schedule->click();
    pump(500);

    auto *dialog = win->findChild<ScheduleDialog *>();
    if (!dialog) {
        // The dialog is a top-level, so it is not a child of the window.
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *d = qobject_cast<ScheduleDialog *>(w)) {
                if (d->isVisible()) {
                    dialog = d;
                    break;
                }
            }
        }
    }

    if (dialog) {
        // Choose which window to delete, when asked to.
        if (auto *box = dialog->findChild<QComboBox *>(QStringLiteral("existingSchedules"))) {
            if (whichToSelect >= 0 && whichToSelect < box->count()) {
                box->setCurrentIndex(whichToSelect);
                emit box->activated(whichToSelect);
                pump(300);
            }
        }

        if (auto *del = dialog->findChild<QPushButton *>(QStringLiteral("deleteSchedule")))
            del->click();
        pump(500);

        // Save, so the caller applies the deletion.
        for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
            if (b->text().startsWith(QLatin1String("Save"))) {
                b->click();
                break;
            }
        }
        pump(500);
    } else {
        std::printf("    (the schedule dialog did not open)\n");
    }

    poll.stop();
    win->close();
    delete win;
    pump(200);

    return store.load().appRules.value(0).windows;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    QTemporaryDir dir;
    if (!dir.isValid())
        return 1;

    // ---- deleting the FIRST window ---------------------------------------
    {
        SettingsStore store(dir.path() + QStringLiteral("/one"));
        Settings s;
        s.appRules.clear();
        AppRule r;
        r.id = 8001;
        r.name = QStringLiteral("Firefox");
        r.exe = QStringLiteral("firefox.exe");
        r.enabled = true;
        r.windows << makeWindow(9001, QStringLiteral("01:00"), QStringLiteral("05:00"));
        r.windows << makeWindow(9002, QStringLiteral("18:00"), QStringLiteral("22:00"));
        s.appRules << r;
        QString err;
        store.save(s, &err);

        std::printf("\n== deleting the FIRST of two ==\n");
        const QVector<Rule> left = deleteShownWindow(store, -1);   // -1: keep the default
        std::printf("    windows remaining: %d\n", int(left.size()));
        for (const Rule &w : left)
            std::printf("      id %llu  %s - %s\n", static_cast<unsigned long long>(w.id),
                        qPrintable(w.from), qPrintable(w.to));

        check(left.size() == 1,
              QStringLiteral("one window remains, not two (%1)").arg(left.size()));
        if (left.size() == 1) {
            check(left.first().id == 9002,
                  QStringLiteral("the SURVIVOR is the second window, id 9002 (got %1)")
                      .arg(left.first().id));
        }
    }

    // ---- deleting the SECOND window --------------------------------------
    {
        SettingsStore store(dir.path() + QStringLiteral("/two"));
        Settings s;
        s.appRules.clear();
        AppRule r;
        r.id = 8002;
        r.name = QStringLiteral("Firefox");
        r.exe = QStringLiteral("firefox.exe");
        r.enabled = true;
        r.windows << makeWindow(9101, QStringLiteral("01:00"), QStringLiteral("05:00"));
        r.windows << makeWindow(9102, QStringLiteral("18:00"), QStringLiteral("22:00"));
        s.appRules << r;
        QString err;
        store.save(s, &err);

        std::printf("\n== deleting the SECOND of two ==\n");
        const QVector<Rule> left = deleteShownWindow(store, 1);
        std::printf("    windows remaining: %d\n", int(left.size()));
        for (const Rule &w : left)
            std::printf("      id %llu  %s - %s\n", static_cast<unsigned long long>(w.id),
                        qPrintable(w.from), qPrintable(w.to));

        check(left.size() == 1,
              QStringLiteral("one window remains, not two (%1)").arg(left.size()));
        if (left.size() == 1) {
            check(left.first().id == 9101,
                  QStringLiteral("the SURVIVOR is the first window, id 9101 (got %1)")
                      .arg(left.first().id));
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_delete_either passed."
                                        : "test_delete_either FAILED.");
    return g_failures == 0 ? 0 : 1;
}
