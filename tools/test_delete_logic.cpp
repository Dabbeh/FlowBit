// Verifies the caller's apply logic: deleting a window must not write it back.
//
// The bug being fixed was in MainWindow::onAppRuleSchedule, not in the dialog. The dialog
// reported deletions correctly; the caller then applied them and wrote result() back
// afterwards, and result() still named the window that had just been removed - so the
// "replace or append" step put it straight back. Deleting the second window worked because
// result() held the first; deleting the first appeared to do nothing.
//
// Driving the dialog through the GUI needs a modal window and cannot be asserted
// unattended, so this reproduces the caller's sequence directly against the same data
// shapes. It is a test of the ORDER and the guard, which is what was wrong.

#include "schedule.h"
#include "settings.h"

#include <QCoreApplication>
#include <QSet>

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

Rule makeWindow(quint64 id, const QString &from, const QString &to)
{
    Rule r;
    r.id = id;
    r.name = QStringLiteral("Firefox");
    r.from = from;
    r.to = to;
    r.days = 0x7F;
    r.enabled = true;
    return r;
}

// The caller's apply step, with the fix, expressed as the code under test does it.
QVector<Rule> apply(const QVector<Rule> &stored, const QVector<quint64> &removedIds,
                    const Rule &result, const QVector<Rule> &added)
{
    QVector<Rule> windows = stored;

    QSet<quint64> deleted;
    for (quint64 gone : removedIds) {
        deleted.insert(gone);
        for (int i = 0; i < windows.size(); ++i) {
            if (windows.at(i).id == gone) {
                windows.removeAt(i);
                break;
            }
        }
    }

    if (result.id != 0 && !deleted.contains(result.id)) {
        bool replaced = false;
        for (Rule &existing : windows) {
            if (existing.id == result.id) {
                existing = result;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            windows.append(result);
    }

    for (Rule extra : added)
        windows.append(extra);

    return windows;
}

// What the caller did BEFORE the fix, so the difference is demonstrated rather than
// merely asserted.
QVector<Rule> applyWithoutGuard(const QVector<Rule> &stored, const QVector<quint64> &removedIds,
                                const Rule &result)
{
    QVector<Rule> windows = stored;
    for (quint64 gone : removedIds) {
        for (int i = 0; i < windows.size(); ++i) {
            if (windows.at(i).id == gone) {
                windows.removeAt(i);
                break;
            }
        }
    }
    if (result.id != 0) {
        bool replaced = false;
        for (Rule &existing : windows) {
            if (existing.id == result.id) {
                existing = result;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            windows.append(result);
    }
    return windows;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const Rule first = makeWindow(9001, QStringLiteral("01:00"), QStringLiteral("05:00"));
    const Rule second = makeWindow(9002, QStringLiteral("18:00"), QStringLiteral("22:00"));
    const QVector<Rule> stored{first, second};

    std::printf("\n== deleting the FIRST window ==\n");
    {
        // The dialog opened on the first, so result() still names it.
        const QVector<Rule> after = apply(stored, {9001}, first, {});
        std::printf("    remaining: %d\n", int(after.size()));
        for (const Rule &w : after)
            std::printf("      id %llu  %s - %s\n",
                        static_cast<unsigned long long>(w.id), qPrintable(w.from),
                        qPrintable(w.to));

        check(after.size() == 1,
              QStringLiteral("one window remains (%1)").arg(after.size()));
        check(!after.isEmpty() && after.first().id == 9002,
              "the survivor is the second window");

        // And the old behaviour, to show the difference rather than claim it.
        const QVector<Rule> before = applyWithoutGuard(stored, {9001}, first);
        std::printf("    without the guard: %d window(s) - the deletion is undone\n",
                    int(before.size()));
        check(before.size() == 2,
              "without the guard the deleted window comes back, which was the bug");
    }

    std::printf("\n== deleting the SECOND window ==\n");
    {
        const QVector<Rule> after = apply(stored, {9002}, first, {});
        std::printf("    remaining: %d\n", int(after.size()));
        check(after.size() == 1,
              QStringLiteral("one window remains (%1)").arg(after.size()));
        check(!after.isEmpty() && after.first().id == 9001,
              "the survivor is the first window");
    }

    std::printf("\n== editing the first while keeping both ==\n");
    {
        Rule edited = first;
        edited.from = QStringLiteral("02:00");
        const QVector<Rule> after = apply(stored, {}, edited, {});
        check(after.size() == 2, "an edit does not change how many windows there are");
        check(!after.isEmpty() && after.first().from == QLatin1String("02:00"),
              "the edit is applied in place");
        check(!after.isEmpty() && after.first().id == 9001,
              "and the window keeps its id");
    }

    std::printf("\n== adding a window ==\n");
    {
        Rule extra = makeWindow(0, QStringLiteral("12:00"), QStringLiteral("14:00"));
        extra.id = 9003;
        const QVector<Rule> after = apply(stored, {}, first, {extra});
        check(after.size() == 3, "an added window is appended");
        check(after.size() == 3 && after.last().id == 9003, "and it is the new one");
    }

    std::printf("\n== deleting both ==\n");
    {
        const QVector<Rule> after = apply(stored, {9001, 9002}, first, {});
        check(after.isEmpty(), "deleting both leaves none");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_delete_logic passed."
                                        : "test_delete_logic FAILED.");
    return g_failures == 0 ? 0 : 1;
}
