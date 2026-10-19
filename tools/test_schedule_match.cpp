// Verifies that schedule windows actually MATCH, using the user's own scenario:
// a limit set from 8pm to 9pm must be active at 20:30 and inactive at 19:00.
//
// This is the check that was missing. Schedules appeared to "do nothing" because
// an unrecognised time in the field made the window never match, silently - and
// nothing anywhere reported that.

#include "schedule.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTextStream>

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

QDateTime at(int year, int month, int day, int hour, int minute)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute));
}

Rule window(const QString &from, const QString &to, quint8 days, bool enabled = true)
{
    Rule r;
    r.id = 1;
    r.name = QStringLiteral("test");
    r.from = from;
    r.to = to;
    r.days = days;
    r.enabled = enabled;
    r.down = 128 * 1024;
    r.up = 128 * 1024;
    return r;
}

const quint8 kEveryDay = 0x7F;   // all seven bits

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    // 2026-10-16 is a Friday; 2026-10-17 a Saturday.
    const QDateTime fridayEvening = at(2026, 10, 16, 20, 30);
    const QDateTime fridayBefore = at(2026, 10, 16, 19, 0);
    const QDateTime fridayNight = at(2026, 10, 16, 22, 0);

    // ---- the user's exact request: a limit from 8pm to 9pm ----------------
    {
        const Rule r = window(QStringLiteral("20:00"), QStringLiteral("21:00"), kEveryDay);
        expect(ruleMatches(r, fridayEvening),
               QStringLiteral("8pm-9pm schedule is ACTIVE at 20:30"));
        expect(!ruleMatches(r, fridayBefore),
               QStringLiteral("8pm-9pm schedule is INACTIVE at 19:00"));
        expect(!ruleMatches(r, fridayNight),
               QStringLiteral("8pm-9pm schedule is INACTIVE at 22:00"));
    }

    // ---- the same window written in the forms a user actually types -------
    // These are the cases that used to fail: the field was capped at 5 characters
    // and the parser understood only H:MM / HHMM.
    {
        const Rule r = window(QStringLiteral("8pm"), QStringLiteral("9pm"), kEveryDay);
        expect(ruleMatches(r, fridayEvening),
               QStringLiteral("'8pm'-'9pm' written in 12-hour form still matches at 20:30"));
        expect(!ruleMatches(r, fridayBefore),
               QStringLiteral("'8pm'-'9pm' is inactive at 19:00"));
    }
    {
        const Rule r = window(QStringLiteral("20"), QStringLiteral("21"), kEveryDay);
        expect(ruleMatches(r, fridayEvening),
               QStringLiteral("bare '20'-'21' (no :00) matches at 20:30"));
    }
    {
        const Rule r = window(QStringLiteral("8:00 pm"), QStringLiteral("9:00 pm"), kEveryDay);
        expect(ruleMatches(r, fridayEvening),
               QStringLiteral("'8:00 pm'-'9:00 pm' matches at 20:30"));
    }
    {
        // 30-minute window inside the hour, the other half of the user's example.
        const Rule r = window(QStringLiteral("20:30"), QStringLiteral("21:00"), kEveryDay);
        expect(ruleMatches(r, at(2026, 10, 16, 20, 30)),
               QStringLiteral("a 20:30 start is inclusive at exactly 20:30"));
        expect(ruleMatches(r, at(2026, 10, 16, 20, 45)),
               QStringLiteral("a 20:30-21:00 window covers 20:45"));
        expect(!ruleMatches(r, at(2026, 10, 16, 21, 0)),
               QStringLiteral("a 20:30-21:00 window excludes exactly 21:00"));
    }

    // ---- overnight windows -------------------------------------------------
    {
        const Rule r = window(QStringLiteral("22:00"), QStringLiteral("07:00"), kEveryDay);
        expect(ruleMatches(r, at(2026, 10, 16, 23, 30)),
               QStringLiteral("overnight window matches before midnight"));
        expect(ruleMatches(r, at(2026, 10, 17, 3, 0)),
               QStringLiteral("overnight window matches after midnight"));
        expect(!ruleMatches(r, at(2026, 10, 16, 12, 0)),
               QStringLiteral("overnight window does not match midday"));
    }

    // ---- day masks ---------------------------------------------------------
    {
        const quint8 monday = 0x01;
        const Rule r = window(QStringLiteral("20:00"), QStringLiteral("21:00"), monday);
        expect(!ruleMatches(r, fridayEvening),
               QStringLiteral("a Monday-only window does not match a Friday"));
    }

    // ---- disabled and malformed rules must not match -----------------------
    {
        const Rule r = window(QStringLiteral("20:00"), QStringLiteral("21:00"), kEveryDay, false);
        expect(!ruleMatches(r, fridayEvening),
               QStringLiteral("a disabled schedule never matches"));
    }
    {
        // Malformed text must be inert, and the app now warns about it in the
        // dialog, so this is the safety net rather than the only defence.
        const Rule r = window(QStringLiteral("banana"), QStringLiteral("21:00"), kEveryDay);
        expect(!ruleMatches(r, fridayEvening),
               QStringLiteral("a window with unparseable text never matches"));
    }

    // ---- end to end through evaluate(): the settings-level decision ---------
    {
        Settings s;
        s.enabled = true;
        s.rules.append(window(QStringLiteral("20:00"), QStringLiteral("21:00"), kEveryDay));
        s.defaultDown = 0;
        s.defaultUp = 0;

        const EffectiveLimits during = evaluate(s, fridayEvening);
        expect(during.throttling,
               QStringLiteral("evaluate(): the 8pm-9pm limit is in force at 20:30"));

        const EffectiveLimits before = evaluate(s, fridayBefore);
        expect(!before.throttling,
               QStringLiteral("evaluate(): no limit is in force at 19:00"));
    }

    out << QStringLiteral("%1 checks, %2 failures\n").arg(checks).arg(failures);
    if (failures == 0)
        out << QStringLiteral("test_schedule passed.\n");
    return failures == 0 ? 0 : 1;
}
