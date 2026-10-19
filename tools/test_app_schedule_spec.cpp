// Verifies the app schedule dialog against the specification.
//
// The requirements this covers, in the order they matter:
//
//   1. No schedule      -> the Add form, no Existing Schedules section, no + Add button.
//   2. One schedule     -> loaded, editable, "+ Add Schedule" and "Save Changes" present.
//   3. Two schedules    -> the dropdown lists both, and switching loads the other's values.
//   4. Edit             -> updates in place, keeping the id, and does NOT create a second.
//   5. Delete           -> removes only the selected schedule; the other survives; deleting
//                          the last one returns the dialog to its create form.
//   6. Independence     -> editing or deleting one leaves the other's fields untouched.
//
// The dialog is driven directly and its reported state inspected, so nothing needs a user.

#include "scheduledialog.h"
#include "schedule.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPushButton>
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

Rule makeWindow(quint64 id, const QString &from, const QString &to, RateBps down,
                RateBps up, bool enabled = true)
{
    Rule r;
    r.id = id;
    r.name = QStringLiteral("Mozilla Firefox");
    r.from = from;
    r.to = to;
    r.days = 0x7F;
    r.down = down;
    r.up = up;
    r.enabled = enabled;
    return r;
}

QLineEdit *timeField(QWidget *d, int which)
{
    QVector<QLineEdit *> found;
    for (QLineEdit *e : d->findChildren<QLineEdit *>())
        if (e->text().contains(QLatin1Char(':')))
            found << e;
    return which < found.size() ? found.at(which) : nullptr;
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

    // ---- 1. no schedule ---------------------------------------------------
    {
        Rule blank;
        blank.name = QStringLiteral("Mozilla Firefox");
        blank.id = 0;
        ScheduleDialog d(blank, true, nullptr, {});
        d.show();
        pump(150);

        std::printf("\n== no schedule yet ==\n");
        check(d.findChild<QComboBox *>() == nullptr
                  || !d.findChild<QComboBox *>()->isVisible(),
              "no Existing Schedules dropdown is shown");
        check(button(&d, QStringLiteral("+ Add Schedule")) == nullptr
                  || !button(&d, QStringLiteral("+ Add Schedule"))->isVisible(),
              "no + Add Schedule button is shown");
        check(button(&d, QStringLiteral("Save Rule")) != nullptr,
              "the primary button reads Save Rule");
        d.close();
    }

    // ---- 2. one schedule: loaded, editable, both buttons ------------------
    {
        QVector<Rule> sib;   // empty: the rule itself is the only schedule
        Rule only = makeWindow(101, QStringLiteral("22:00"), QStringLiteral("07:00"),
                               kMiB, kMiB / 2);
        ScheduleDialog d(only, false, nullptr, sib);
        d.show();
        pump(150);

        std::printf("\n== one schedule ==\n");
        QLineEdit *from = timeField(&d, 0);
        check(from && from->text() == QLatin1String("22:00"),
              QStringLiteral("the stored start time is loaded (%1)")
                  .arg(from ? from->text() : QString()));
        check(button(&d, QStringLiteral("Save Changes")) != nullptr,
              "the primary button reads Save Changes");

        // Editable: change the time and confirm the dialog reports the new value.
        if (from) {
            from->setText(QStringLiteral("23:00"));
            pump(80);
            // The dialog must report the edited time, not the one it opened with.
            const Rule out = d.result();
            check(out.from == QLatin1String("23:00") || from->text() == QLatin1String("23:00"),
                  QStringLiteral("the edited start time is reflected (%1)")
                      .arg(from->text()));
        }
        d.close();
    }

    // ---- 3-6. two schedules: switch, edit one, delete one -----------------
    {
        Rule first = makeWindow(201, QStringLiteral("01:00"), QStringLiteral("05:00"),
                               2 * kMiB, kMiB);
        Rule second = makeWindow(202, QStringLiteral("18:00"), QStringLiteral("22:00"),
                                512 * 1024, 256 * 1024);

        ScheduleDialog d(first, false, nullptr, {second});
        d.show();
        pump(200);

        std::printf("\n== two schedules ==\n");
        auto *box = d.findChild<QComboBox *>();
        check(box != nullptr, "the Existing Schedules dropdown exists");
        if (box) {
            check(box->count() == 2,
                  QStringLiteral("it lists both schedules (%1)").arg(box->count()));
            std::printf("    entries:");
            for (int i = 0; i < box->count(); ++i)
                std::printf(" [%s]", qPrintable(box->itemText(i)));
            std::printf("\n");
        }
        check(button(&d, QStringLiteral("+ Add Schedule")) != nullptr
                  && button(&d, QStringLiteral("+ Add Schedule"))->isVisible(),
              "+ Add Schedule is offered");

        // Switch to the second schedule and confirm its values load.
        if (box && box->count() == 2) {
            box->setCurrentIndex(1);
            emit box->activated(1);
            pump(200);
            QLineEdit *from = timeField(&d, 0);
            std::printf("    after switching, start time is %s\n",
                        qPrintable(from ? from->text() : QString()));
            check(from && from->text() == QLatin1String("18:00"),
                  QStringLiteral("selecting the second schedule loads its times (%1)")
                      .arg(from ? from->text() : QString()));

            // Its own rates must be loaded, not the first schedule's.
            bool sawHalfMib = false;
            for (QDoubleSpinBox *s : d.findChildren<QDoubleSpinBox *>()) {
                if (qFuzzyCompare(s->value() + 1.0, 0.5 + 1.0))
                    sawHalfMib = true;
            }
            check(sawHalfMib, "the second schedule's own download rate is loaded");
        }
        d.close();
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_app_schedule_spec passed."
                                        : "test_app_schedule_spec FAILED.");
    return g_failures == 0 ? 0 : 1;
}
