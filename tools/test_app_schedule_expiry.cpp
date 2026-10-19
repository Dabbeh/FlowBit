// Verifies per-app schedule expiry: the reported defect was that a limit set for
// 11:40-11:42 kept applying after 11:42. The window must be active INSIDE the
// window and inactive AFTER it, and the evaluated state must carry zero limits
// when inactive so the engine's buckets get released.

#include "applimitengine.h"
#include "schedule.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTextStream>
#include <QColor>

using namespace net;

namespace {

int failures = 0;
int checks = 0;

void expect(bool condition, const QString &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        QTextStream(stdout) << QStringLiteral("  FAIL  %1\n").arg(what);
    }
}

AppRule makeRule(const QString &from, const QString &to)
{
    AppRule r;
    r.id = 7;
    r.name = QStringLiteral("App schedule");
    r.exe = QStringLiteral("app.exe");
    r.enabled = true;
    r.down = 50 * 1024;    // 50 KB/s, like the user's test
    r.up = 50 * 1024;
    Rule w;
    w.id = 8;
    w.from = from;
    w.to = to;
    w.days = 0x7F;         // every day
    w.enabled = true;
    r.windows.append(w);
    return r;
}

QDateTime at(int h, int m)
{
    return QDateTime(QDate(2026, 10, 16), QTime(h, m));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    // The user's exact scenario: 11:40 to 11:42.
    const AppRule rule = makeRule(QStringLiteral("11:40"), QStringLiteral("11:42"));

    // INSIDE the window: active, carrying the configured rates.
    {
        const AppLimitState s = AppLimitEngine::evaluate(rule, at(11, 40));
        expect(s.active, QStringLiteral("11:40-11:42 rule is ACTIVE at 11:40"));
        expect(s.active && s.down == rule.down && s.up == rule.up,
               QStringLiteral("inside the window the state carries the limit rates"));
    }
    {
        const AppLimitState s = AppLimitEngine::evaluate(rule, at(11, 41));
        expect(s.active, QStringLiteral("11:40-11:42 rule is ACTIVE at 11:41"));
    }

    // OUTSIDE the window — this is the reported defect. The state must be inactive
    // AND must not carry the limit rates, because the engine erases the bucket of
    // an inactive rule: with non-zero rates here, the limit would persist past the
    // end of the window.
    {
        const AppLimitState s = AppLimitEngine::evaluate(rule, at(11, 42));
        expect(!s.active, QStringLiteral("11:40-11:42 rule is INACTIVE at exactly 11:42"));
        expect(s.down == 0 && s.up == 0,
               QStringLiteral("inactive state carries NO limit rates"));
    }
    {
        const AppLimitState s = AppLimitEngine::evaluate(rule, at(11, 45));
        expect(!s.active, QStringLiteral("11:40-11:42 rule is INACTIVE at 11:45"));
        expect(s.down == 0 && s.up == 0,
               QStringLiteral("well past the window: still no rates"));
    }

    // One minute before the window: also inactive.
    {
        const AppLimitState s = AppLimitEngine::evaluate(rule, at(11, 39));
        expect(!s.active, QStringLiteral("the rule is inactive before 11:40"));
    }

    out << QStringLiteral("%1 checks, %2 failures\n").arg(checks).arg(failures);
    if (failures == 0)
        out << QStringLiteral("test_app_schedule_expiry passed.\n");
    return failures == 0 ? 0 : 1;
}
