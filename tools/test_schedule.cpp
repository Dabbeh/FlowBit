// Standalone test for schedule evaluation + unit conversion (no GUI, no driver).
// Build/run: tools/run_tests.ps1
#include "../src/schedule.h"
#include "../src/units.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QTime>

#include <cmath>
#include <cstdio>

using namespace net;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", qPrintable(what));
    }
}

static QString describe(const EffectiveLimits &l)
{
    return QStringLiteral("down=%1 up=%2 throttle=%3 rule=%4")
        .arg(l.down)
        .arg(l.up)
        .arg(l.throttling ? "yes" : "no")
        .arg(l.ruleIndex);
}

// Builds a QDateTime on a known weekday so the day-mask tests are deterministic.
// 2026-10-14 is a Wednesday.
static QDateTime wed(int hour, int minute)
{
    return QDateTime(QDate(2026, 10, 14), QTime(hour, minute));
}

static QDateTime sat(int hour, int minute)
{
    return QDateTime(QDate(2026, 10, 17), QTime(hour, minute));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // -- sanity: the fixtures really are the weekdays we assume ------------
    check(QDate(2026, 10, 14).dayOfWeek() == Qt::Wednesday, "fixture 2026-10-14 is Wednesday");
    check(QDate(2026, 10, 17).dayOfWeek() == Qt::Saturday, "fixture 2026-10-17 is Saturday");

    // -- parseClock ---------------------------------------------------------
    check(parseClock("22:00") == QTime(22, 0), "parse 22:00");
    check(parseClock("7:05") == QTime(7, 5), "parse 7:05");
    check(parseClock("0705") == QTime(7, 5), "parse 0705");
    check(parseClock("2359") == QTime(23, 59), "parse 2359");
    check(!parseClock("24:00").isValid(), "reject 24:00");
    check(!parseClock("22:60").isValid(), "reject 22:60");
    check(!parseClock("").isValid(), "reject empty");
    check(!parseClock("garbage").isValid(), "reject garbage");

    // -- windowContains -----------------------------------------------------
    check(windowContains(22 * 60, 7 * 60, 23 * 60), "23:00 inside 22:00-07:00");
    check(windowContains(22 * 60, 7 * 60, 2 * 60), "02:00 inside 22:00-07:00");
    check(!windowContains(22 * 60, 7 * 60, 12 * 60), "12:00 outside 22:00-07:00");
    check(!windowContains(22 * 60, 7 * 60, 7 * 60), "07:00 is exclusive end");
    check(windowContains(22 * 60, 7 * 60, 22 * 60), "22:00 is inclusive start");
    check(windowContains(9 * 60, 17 * 60, 12 * 60), "12:00 inside 09:00-17:00");
    check(!windowContains(9 * 60, 17 * 60, 8 * 60), "08:00 outside 09:00-17:00");
    check(windowContains(0, 0, 13 * 60), "from==to means all day");

    // -- day masks ----------------------------------------------------------
    check(maskContainsDay(0x7F, Qt::Monday), "daily mask includes Monday");
    check(maskContainsDay(0x7F, Qt::Sunday), "daily mask includes Sunday");
    check(maskContainsDay(0x1F, Qt::Friday), "Mon-Fri includes Friday");
    check(!maskContainsDay(0x1F, Qt::Saturday), "Mon-Fri excludes Saturday");
    check(maskContainsDay(0x60, Qt::Saturday), "weekend mask includes Saturday");
    check(!maskContainsDay(0x60, Qt::Wednesday), "weekend mask excludes Wednesday");
    check(maskFromQtDay(Qt::Monday) == 0x01, "Monday maps to bit 0");
    check(maskFromQtDay(Qt::Sunday) == 0x40, "Sunday maps to bit 6");

    // -- evaluate: disabled master switch ----------------------------------
    {
        Settings s;
        s.enabled = false;
        s.defaultDown = 1024;
        const EffectiveLimits l = evaluate(s, wed(12, 0));
        check(!l.throttling, "disabled => not throttling");
        check(l.down == 0 && l.up == 0, "disabled => zero limits");
    }

    // -- evaluate: defaults when no rule matches ---------------------------
    {
        Settings s;
        s.enabled = true;
        s.defaultDown = kUnlimited;
        s.defaultUp = kUnlimited;

        Rule night;
        night.name = QStringLiteral("Night");
        night.from = "22:00";
        night.to = "07:00";
        night.down = 2 * 1024 * 1024;
        night.up = 512 * 1024;
        s.rules.append(night);

        const EffectiveLimits day = evaluate(s, wed(12, 0));
        check(!day.throttling && day.ruleIndex == -1,
              QStringLiteral("midday falls back to defaults: ") + describe(day));

        const EffectiveLimits evening = evaluate(s, wed(23, 30));
        check(evening.throttling && evening.down == 2 * 1024 * 1024 && evening.ruleIndex == 0,
              QStringLiteral("23:30 uses the night rule: ") + describe(evening));

        const EffectiveLimits early = evaluate(s, wed(6, 0));
        check(early.throttling, QStringLiteral("06:00 inside overnight rule: ") + describe(early));

        const EffectiveLimits justAfter = evaluate(s, wed(7, 0));
        check(!justAfter.throttling && justAfter.ruleIndex == -1,
              QStringLiteral("07:00 releases the limit: ") + describe(justAfter));
    }

    // -- evaluate: weekday masks gate the window ---------------------------
    {
        Settings s;
        s.enabled = true;

        Rule workHours;
        workHours.name = QStringLiteral("Work Hours");
        workHours.from = "08:00";
        workHours.to = "17:00";
        workHours.down = 1 * 1024 * 1024;
        workHours.up = 256 * 1024;
        workHours.days = 0x1F;  // Mon-Fri
        s.rules.append(workHours);

        const EffectiveLimits wednesday = evaluate(s, wed(10, 0));
        check(wednesday.throttling && wednesday.ruleIndex == 0,
              QStringLiteral("Wed 10:00 matches Mon-Fri rule: ") + describe(wednesday));

        const EffectiveLimits saturday = evaluate(s, sat(10, 0));
        check(!saturday.throttling && saturday.ruleIndex == -1,
              QStringLiteral("Sat 10:00 does NOT match Mon-Fri rule: ") + describe(saturday));
    }

    // -- an overnight window carries its day mask past midnight -------------
    //
    // The after-midnight tail belongs to the day the window STARTED on. Checking the mask
    // against the current day meant a Monday-only 22:00 -> 07:00 rule stopped dead at
    // midnight and never covered Tuesday's early hours - configured, displayed correctly, and
    // never applied.
    {
        Settings s;
        s.enabled = true;

        Rule mondayNight;
        mondayNight.name = QStringLiteral("Monday Night");
        mondayNight.from = "22:00";
        mondayNight.to = "07:00";
        mondayNight.down = 1 * 1024 * 1024;
        mondayNight.days = 0x01;   // Monday only
        s.rules.append(mondayNight);

        check(QDate(2026, 10, 12).dayOfWeek() == Qt::Monday, "fixture 2026-10-12 is Monday");

        const auto day = [](int dayOfMonth, int hour, int minute) {
            return QDateTime(QDate(2026, 10, dayOfMonth), QTime(hour, minute));
        };

        const EffectiveLimits monLate = evaluate(s, day(12, 23, 0));
        check(monLate.throttling && monLate.ruleIndex == 0,
              QStringLiteral("Monday 23:00 is inside a Monday overnight window: ")
                  + describe(monLate));

        const EffectiveLimits tueEarly = evaluate(s, day(13, 3, 0));
        check(tueEarly.throttling && tueEarly.ruleIndex == 0,
              QStringLiteral("Tuesday 03:00 is still inside it, as the tail of Monday: ")
                  + describe(tueEarly));

        const EffectiveLimits tueAtEnd = evaluate(s, day(13, 7, 0));
        check(!tueAtEnd.throttling,
              QStringLiteral("Tuesday 07:00 is the exclusive end: ") + describe(tueAtEnd));

        const EffectiveLimits tueNight = evaluate(s, day(13, 23, 0));
        check(!tueNight.throttling,
              QStringLiteral("Tuesday night is not covered by a Monday-only window: ")
                  + describe(tueNight));

        const EffectiveLimits monEarly = evaluate(s, day(12, 3, 0));
        check(!monEarly.throttling,
              QStringLiteral("Monday 03:00 belongs to Sunday's window, which is not set: ")
                  + describe(monEarly));
    }

    // -- evaluate: first matching rule wins, disabled rules skipped --------
    {
        Settings s;
        s.enabled = true;
        s.defaultDown = 999;
        s.defaultUp = 999;

        Rule morning;
        morning.name = QStringLiteral("Morning");
        morning.from = "08:00";
        morning.to = "12:00";
        morning.down = 100;
        morning.up = 100;

        Rule allDayOff;
        allDayOff.from = "00:00";
        allDayOff.to = "00:00";
        allDayOff.down = 500;
        allDayOff.up = 500;
        allDayOff.enabled = false;

        Rule evening;
        evening.name = QStringLiteral("Evening");
        evening.from = "18:00";
        evening.to = "06:00";
        evening.down = 300;
        evening.up = 300;

        s.rules << morning << allDayOff << evening;

        const EffectiveLimits at10 = evaluate(s, wed(10, 0));
        check(at10.ruleIndex == 0 && at10.down == 100,
              QStringLiteral("first enabled match wins: ") + describe(at10));

        const EffectiveLimits at20 = evaluate(s, wed(20, 0));
        check(at20.ruleIndex == 2 && at20.down == 300,
              QStringLiteral("disabled all-day rule skipped: ") + describe(at20));

        const EffectiveLimits at15 = evaluate(s, wed(15, 0));
        check(at15.ruleIndex == -1 && at15.down == 999,
              QStringLiteral("no match => defaults: ") + describe(at15));
    }

    // -- evaluate: a matching rule with 0/0 means uncapped ------------------
    {
        Settings s;
        s.enabled = true;
        s.defaultDown = 500;
        s.defaultUp = 500;

        Rule freeWindow;
        freeWindow.from = "02:00";
        freeWindow.to = "05:00";
        freeWindow.down = kUnlimited;
        freeWindow.up = kUnlimited;
        s.rules.append(freeWindow);

        const EffectiveLimits l = evaluate(s, wed(3, 0));
        check(l.ruleIndex == 0 && !l.throttling && l.down == 0,
              QStringLiteral("rule override to uncapped: ") + describe(l));
    }

    // -- evaluate: malformed rule times never match ------------------------
    {
        Settings s;
        s.enabled = true;
        s.defaultDown = 42;

        Rule broken;
        broken.from = "nonsense";
        broken.to = "07:00";
        broken.down = 999;
        s.rules.append(broken);

        const EffectiveLimits l = evaluate(s, wed(3, 0));
        check(l.ruleIndex == -1 && l.down == 42,
              QStringLiteral("malformed rule ignored: ") + describe(l));
    }

    // -- units: round-trips must not drift ---------------------------------
    {
        struct Case {
            SpeedUnit unit;
            double value;
        };
        const Case cases[] = {
            {SpeedUnit::MibPerSec, 2.50},
            {SpeedUnit::KibPerSec, 500.0},
            {SpeedUnit::BytesPerSec, 4096.0},
            {SpeedUnit::MibPerSec, 0.5},
            {SpeedUnit::KibPerSec, 64.0},
        };

        for (const Case &c : cases) {
            const RateBps bps = toBytesPerSec(c.value, c.unit);
            const double back = fromBytesPerSec(bps, c.unit);
            check(std::fabs(back - c.value) < 0.01,
                  QStringLiteral("round-trip %1 %2 -> %3 B/s -> %4")
                      .arg(c.value, 0, 'f', 2)
                      .arg(speedUnitLabel(c.unit))
                      .arg(bps)
                      .arg(back, 0, 'f', 4));
        }

        // The MB/s <-> KB/s relationship is the one users will notice.
        check(toBytesPerSec(1.0, SpeedUnit::MibPerSec)
                  == toBytesPerSec(1024.0, SpeedUnit::KibPerSec),
              "1 MB/s equals 1024 KB/s");

        // Zero always means uncapped, whichever unit is in play.
        check(toBytesPerSec(0.0, SpeedUnit::MibPerSec) == kUnlimited, "0 MB/s => uncapped");
        check(toBytesPerSec(0.0, SpeedUnit::KibPerSec) == kUnlimited, "0 KB/s => uncapped");

        // Unit auto-selection picks a readable scale.
        check(bestUnitFor(5 * 1024 * 1024) == SpeedUnit::MibPerSec, "5 MB/s picks MB/s");
        check(bestUnitFor(500 * 1024) == SpeedUnit::KibPerSec, "500 KB/s picks KB/s");
        check(bestUnitFor(900) == SpeedUnit::BytesPerSec, "900 B/s picks B/s");

        // Formatting.
        check(formatRate(kUnlimited) == QStringLiteral("Uncapped"), "0 renders as Uncapped");
        check(formatRate(2 * 1024 * 1024).contains(QStringLiteral("MB/s")), "MB/s label");
        check(formatRate(500 * 1024).contains(QStringLiteral("KB/s")), "KB/s label");
        check(formatBytes(142.5 * 1024 * 1024 * 1024, 1) == QStringLiteral("142.5 GB"),
              "GB volume formatting");

        // Day description helpers used by the rule rows.
        check(describeDays(0x7F) == QStringLiteral("Daily"), "0x7F => Daily");
        check(describeDays(0x1F) == QStringLiteral("Mon-Fri"), "0x1F => Mon-Fri");
        check(describeDays(0x60) == QStringLiteral("Sat-Sun"), "0x60 => Sat-Sun");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
