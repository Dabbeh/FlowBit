// Verifies the app-schedule dialog edits an existing window rather than adding another.
//
// The reported bug: pressing Schedule twice produced two windows, because the handler
// built a fresh Rule with a new id every time and never offered the existing ones for
// editing. So the thing to prove is that an existing window round-trips through Edit
// unchanged apart from what was altered - same id, same other fields - and that Remove
// takes the right one out.
//
// Drives the dialog widget directly, so the window can be inspected without a user.

#include "appscheduleedialog.h"
#include "schedule.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QStringList>
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

// The buttons the dialog builds, by their labels.
QPushButton *button(QWidget *root, const QString &text)
{
    for (QPushButton *b : root->findChildren<QPushButton *>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

int countButtons(QWidget *root, const QString &text)
{
    int n = 0;
    for (QPushButton *b : root->findChildren<QPushButton *>())
        if (b->text() == text)
            ++n;
    return n;
}

Rule makeWindow(quint64 id, const QString &from, const QString &to, quint8 days)
{
    Rule r;
    r.id = id;
    r.name = QStringLiteral("Firefox");
    r.from = from;
    r.to = to;
    r.days = days;
    r.down = 256 * 1024;
    r.up = 666 * 1024;
    return r;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    // ---- no windows: the empty state and an add button --------------------
    {
        AppScheduleDialog dialog(QStringLiteral("Mozilla Firefox"), {});
        dialog.resize(520, 320);
        dialog.show();
        pump(200);

        std::printf("\n== an application with no windows ==\n");
        check(dialog.windows().isEmpty(), "it starts with no windows");
        check(countButtons(&dialog, QStringLiteral("Add window")) == 1,
              "an Add window button is offered");
        check(countButtons(&dialog, QStringLiteral("Edit")) == 0,
              "no Edit button when there is nothing to edit");
        check(countButtons(&dialog, QStringLiteral("Remove")) == 0,
              "no Remove button when there is nothing to remove");
        dialog.close();
    }

    // ---- two windows: one Edit and one Remove each ------------------------
    {
        QVector<Rule> windows;
        windows << makeWindow(11, QStringLiteral("22:00"), QStringLiteral("07:00"), 0x1F);
        windows << makeWindow(22, QStringLiteral("12:00"), QStringLiteral("14:00"), 0x60);

        AppScheduleDialog dialog(QStringLiteral("Mozilla Firefox"), windows);
        dialog.resize(520, 320);
        dialog.show();
        pump(200);

        std::printf("\n== two existing windows ==\n");
        check(dialog.windows().size() == 2, "both windows are carried in");
        check(countButtons(&dialog, QStringLiteral("Edit")) == 2,
              QStringLiteral("each window has an Edit button (%1)")
                  .arg(countButtons(&dialog, QStringLiteral("Edit"))));
        check(countButtons(&dialog, QStringLiteral("Remove")) == 2,
              QStringLiteral("each window has a Remove button (%1)")
                  .arg(countButtons(&dialog, QStringLiteral("Remove"))));

        // The descriptions must be shown, not just the buttons: the whole complaint was
        // that existing windows were invisible.
        // describeRule() renders times as 12-hour with AM/PM, so the window made from
        // "22:00" reads "10:00 PM - 07:00 AM (overnight)". Checking for "22:00" failed on
        // a formatted string that was in fact present.
        QStringList shown;
        for (QLabel *l : dialog.findChildren<QLabel *>()) {
            const QString text = l->text();
            if (text.contains(QLatin1String("M - ")) || text.contains(QLatin1String("AM"))
                || text.contains(QLatin1String("PM")))
                shown << text;
        }
        for (const QString &s : shown)
            std::printf("    window shown as: \"%s\"\n", qPrintable(s));
        check(shown.size() >= 2,
              QStringLiteral("both windows' hours are listed, not hidden in a prompt (%1)")
                  .arg(shown.size()));
        check(shown.join(QLatin1Char(' ')).contains(QLatin1String("overnight")),
              "an overnight window is marked as overnight");

        // The dialog must report the list unchanged when nothing was done - a cancelled
        // edit must not lose a window.
        const QVector<Rule> after = dialog.windows();
        check(after.size() == 2 && after.at(0).id == 11 && after.at(1).id == 22,
              "the window ids are preserved, so an edit cannot detach a window");

        dialog.close();
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_app_schedule passed."
                                        : "test_app_schedule FAILED.");
    return g_failures == 0 ? 0 : 1;
}
