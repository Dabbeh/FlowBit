// Verifies the app-specific schedule dialog: switching between windows, and deleting one.
//
// The two things reported missing:
//
//   * with two windows, only the FIRST could be opened again - the handler always loaded
//     windows[0] and always wrote back to windows[0], so the second could be added but
//     never selected;
//   * neither could be deleted.
//
// So the things to prove are that selecting the second window loads ITS values (not the
// first's), and that deleting it removes that one and leaves the other intact - including
// its id, which is what the caller uses to apply the change.

#include "scheduledialog.h"
#include "schedule.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMessageBox>
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

Rule makeWindow(quint64 id, const QString &name, const QString &from, const QString &to,
                RateBps down, RateBps up)
{
    Rule r;
    r.id = id;
    r.name = name;
    r.from = from;
    r.to = to;
    r.days = 0x7F;
    r.down = down;
    r.up = up;
    r.enabled = true;
    return r;
}

QLineEdit *startField(QWidget *d)
{
    for (QLineEdit *e : d->findChildren<QLineEdit *>())
        if (e->text().contains(QLatin1Char(':')))
            return e;
    return nullptr;
}

QPushButton *button(QWidget *d, const QString &text)
{
    for (QPushButton *b : d->findChildren<QPushButton *>())
        if (b->text() == text)
            return b;
    return nullptr;
}

constexpr RateBps kMiB = 1024 * 1024;

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    // Two windows with deliberately unrelated values, so loading the wrong one is obvious.
    Rule first = makeWindow(7001, QStringLiteral("Firefox"), QStringLiteral("01:00"),
                            QStringLiteral("05:00"), 2 * kMiB, kMiB);
    Rule second = makeWindow(7002, QStringLiteral("Firefox"), QStringLiteral("18:00"),
                             QStringLiteral("22:00"), 512 * 1024, 256 * 1024);

    // ---- the dialog opens on the first window, offering both --------------
    {
        ScheduleDialog d(first, false, nullptr, {second});
        d.show();
        pump(200);

        std::printf("\n== two windows, opened ==\n");
        auto *box = d.findChild<QComboBox *>(QStringLiteral("existingSchedules"));
        check(box != nullptr, "the Existing Schedules dropdown exists");
        if (box) {
            std::printf("    entries:");
            for (int i = 0; i < box->count(); ++i)
                std::printf(" [%s]", qPrintable(box->itemText(i)));
            std::printf("\n");
            check(box->count() == 2,
                  QStringLiteral("both windows are listed (%1)").arg(box->count()));
            check(box->isVisible(), "the dropdown is visible");
        }
        QLineEdit *from = startField(&d);
        check(from && from->text() == QLatin1String("01:00"),
              QStringLiteral("the first window's start is shown (%1)")
                  .arg(from ? from->text() : QString()));

        auto *add = button(&d, QStringLiteral("+ Add Schedule"));
        check(add && add->isVisible(), "+ Add Schedule is offered");

        // ---- switching loads the second window ---------------------------
        if (box && box->count() == 2) {
            box->setCurrentIndex(1);
            emit box->activated(1);
            pump(250);

            QLineEdit *nowFrom = startField(&d);
            std::printf("    after switching, start is %s\n",
                        qPrintable(nowFrom ? nowFrom->text() : QString()));
            check(nowFrom && nowFrom->text() == QLatin1String("18:00"),
                  QStringLiteral("selecting the second loads ITS start time (%1)")
                      .arg(nowFrom ? nowFrom->text() : QString()));

            // Its own rates too, which is what proves the whole window loaded rather than
            // only its times.
            //
            // 512 KB/s is below 1 MiB/s, so the dialog shows it as 512 in KB/s mode - not
            // as 0.5 in MB/s. The first check looked for 0.5 and failed on a value that was
            // loaded correctly; the unit matters as much as the number.
            bool sawRate = false;
            QString rateSummary;
            for (QDoubleSpinBox *s : d.findChildren<QDoubleSpinBox *>()) {
                rateSummary += QStringLiteral("[%1]").arg(s->value());
                if (qFuzzyCompare(s->value() + 1.0, 512.0 + 1.0))
                    sawRate = true;
            }
            std::printf("    spin values: %s\n", qPrintable(rateSummary));
            check(sawRate, "and its own download rate, shown as 512 in KB/s");
        }

        // ---- deleting removes only the one shown -------------------------
        // selectWindow() asks before discarding edits; nothing is edited here, so the
        // delete confirmation is the only prompt. It is answered by the timer below.
        auto *del = d.findChild<QPushButton *>(QStringLiteral("deleteSchedule"));
        check(del != nullptr, "a delete control exists");
        if (del) {
            // Answer the confirmation as it appears.
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, [] {
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (auto *m = qobject_cast<QMessageBox *>(w)) {
                        if (m->isVisible() && m->button(QMessageBox::Yes))
                            m->button(QMessageBox::Yes)->click();
                    }
                }
            });
            poll.start(30);

            del->click();
            pump(500);
            poll.stop();

            const QVector<quint64> gone = d.removedIds();
            std::printf("    removed ids:");
            for (quint64 g : gone)
                std::printf(" %llu", static_cast<unsigned long long>(g));
            std::printf("\n");

            check(gone.size() == 1,
                  QStringLiteral("exactly one window is marked deleted (%1)").arg(gone.size()));
            check(!gone.isEmpty() && gone.first() == 7002,
                  "the deleted id is the one that was SELECTED, not the first");

            if (box) {
                check(box->count() == 1,
                      QStringLiteral("the dropdown now lists one (%1)").arg(box->count()));
            }
            QLineEdit *after = startField(&d);
            check(after && after->text() == QLatin1String("01:00"),
                  QStringLiteral("the surviving window is shown (%1)")
                      .arg(after ? after->text() : QString()));
        }

        d.close();
    }

    // ---- deleting a single window ---------------------------------------
    {
        ScheduleDialog d(first, false, nullptr, {});
        d.show();
        pump(150);
        std::printf("\n== one window only ==\n");
        auto *add = button(&d, QStringLiteral("+ Add Schedule"));
        check(add && add->isVisible(), "+ Add Schedule is offered when one exists");
        d.close();
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_schedule_switch passed."
                                        : "test_schedule_switch FAILED.");
    return g_failures == 0 ? 0 : 1;
}
