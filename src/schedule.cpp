#include "schedule.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>

#include <atomic>

namespace net {

// ---------------------------------------------------------------------------
// Rule identity
// ---------------------------------------------------------------------------

namespace {

// Starts at 1 and only ever increases. Ids are persisted, so ensureRuleIds()
// also advances this past whatever the loaded settings already contain - an id
// that already exists on disk must never be handed out again.
std::atomic<quint64> g_ruleIdCounter{1};

} // namespace

quint64 nextRuleId()
{
    return g_ruleIdCounter.fetch_add(1);
}

bool ensureRuleIds(Settings &settings)
{
    bool changed = false;
    quint64 highest = g_ruleIdCounter.load();

    for (Rule &rule : settings.rules) {
        if (rule.id == 0) {
            rule.id = nextRuleId();
            changed = true;
        }
        highest = qMax(highest, rule.id);
    }

    // Per-application rules carry ids from the same sequence, and each of their
    // schedule windows does too. Sharing one counter means an id is unique across
    // the whole settings file, so a window can never collide with a rule.
    for (AppRule &app : settings.appRules) {
        if (app.id == 0) {
            app.id = nextRuleId();
            changed = true;
        }
        highest = qMax(highest, app.id);
        for (Rule &w : app.windows) {
            if (w.id == 0) {
                w.id = nextRuleId();
                changed = true;
            }
            highest = qMax(highest, w.id);
        }
    }

    // Keep the counter ahead of everything loaded, so a later "new rule" cannot
    // collide with an id restored from the settings file.
    quint64 expected = highest + 1;
    quint64 current = g_ruleIdCounter.load();
    while (current < expected && !g_ruleIdCounter.compare_exchange_weak(current, expected)) {
        // compare_exchange_weak updates `current` on failure; retry until it
        // succeeds or another thread has already advanced the counter far enough.
    }

    return changed;
}

int ruleIndexById(const Settings &settings, quint64 id)
{
    if (id == 0)
        return -1;
    for (int i = 0; i < settings.rules.size(); ++i) {
        if (settings.rules.at(i).id == id)
            return i;
    }
    return -1;
}

int appRuleIndexById(const Settings &settings, quint64 id)
{
    if (id == 0)
        return -1;
    for (int i = 0; i < settings.appRules.size(); ++i) {
        if (settings.appRules.at(i).id == id)
            return i;
    }
    return -1;
}

QTime parseClock(const QString &text)
{
    // Accepts the forms a person actually types. Previously only "H:MM" and
    // "HHMM" were understood, which meant "9pm", "9 pm", "9:30 pm" and even a
    // bare "21" were rejected - and a rejected time makes a schedule window
    // silently NEVER match, with no error shown anywhere. That is what made
    // schedules appear to do nothing at all.
    //
    // Now understood (case-insensitive, surrounding spaces ignored):
    //     21          21:00      21:30      2130
    //     9           9:30       9pm        9:30pm
    //     9 pm        9:30 PM    12am       12pm
    QString s = text.trimmed().toLower();
    if (s.isEmpty())
        return QTime();

    // Trailing am/pm, with or without a space. "12am" is midnight and "12pm" is
    // noon, so noon/midnight need the 12-to-0 correction below.
    bool sawAm = false;
    bool sawPm = false;
    static const QRegularExpression ampm(QStringLiteral(R"(\s*([ap])\.?\s*m\.?\s*$)"));
    const QRegularExpressionMatch am = ampm.match(s);
    if (am.hasMatch()) {
        sawAm = (am.captured(1) == QLatin1String("a"));
        sawPm = !sawAm;
        s = s.left(am.capturedStart()).trimmed();
        // Tolerate the "9.30" form some people type for 9:30.
        s.replace(QLatin1Char('.'), QLatin1Char(':'));
    }

    // Separator may be ':' or, for the compact form, nothing at all.
    QString hoursPart;
    QString minutesPart;
    const int colon = s.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        hoursPart = s.left(colon).trimmed();
        minutesPart = s.mid(colon + 1).trimmed();
        // Reject anything with a second separator: "9:3:2" must not be silently
        // read as 9:00 by ignoring the trailing text.
        if (minutesPart.contains(QLatin1Char(':')))
            return QTime();
        // The minute field is at most two digits; "9:300" is not 9:00.
        if (minutesPart.size() > 2)
            return QTime();
    } else if (s.size() <= 4 && !s.isEmpty()) {
        // "21" -> hour only; "2130" -> hour and minute.
        if (s.size() <= 2) {
            hoursPart = s;
            minutesPart = QStringLiteral("0");
        } else {
            hoursPart = s.left(s.size() - 2);
            minutesPart = s.right(2);
        }
    } else {
        return QTime();
    }

    bool okH = false;
    bool okM = false;
    int h = hoursPart.toInt(&okH);
    int m = minutesPart.isEmpty() ? 0 : minutesPart.toInt(&okM);
    if (!okH || (m != 0 && !okM))
        return QTime();

    if (sawAm || sawPm) {
        // 12-hour clock: the hour must be 1..12, and 12 wraps to 0.
        if (h < 1 || h > 12)
            return QTime();
        if (h == 12)
            h = 0;
        if (sawPm)
            h += 12;
    }

    if (h < 0 || h > 23 || m < 0 || m > 59)
        return QTime();
    return QTime(h, m);
}

QString formatClock(const QTime &t)
{
    // Canonical storage form. Always HH:MM so stored settings stay stable no
    // matter which input style produced them.
    if (!t.isValid())
        return QString();
    return QStringLiteral("%1:%2")
        .arg(t.hour(), 2, 10, QLatin1Char('0'))
        .arg(t.minute(), 2, 10, QLatin1Char('0'));
}

int minutesOfDay(const QTime &t)
{
    if (!t.isValid())
        return -1;
    return t.hour() * 60 + t.minute();
}

bool windowContains(int fromMinutes, int toMinutes, int nowMinutes)
{
    if (fromMinutes < 0 || toMinutes < 0 || nowMinutes < 0)
        return false;
    if (fromMinutes == toMinutes)
        return true;  // whole day
    if (fromMinutes < toMinutes)
        return nowMinutes >= fromMinutes && nowMinutes < toMinutes;
    // Overnight window: 22:00 -> 07:00
    return nowMinutes >= fromMinutes || nowMinutes < toMinutes;
}

QString describeRule(const Rule &rule, int index)
{
    const QTime from = parseClock(rule.from);
    const QTime to = parseClock(rule.to);
    const QString label = rule.name.isEmpty()
                              ? QStringLiteral("Rule %1").arg(index + 1)
                              : rule.name;

    if (!from.isValid() || !to.isValid())
        return QStringLiteral("%1: invalid time").arg(label);

    QString shape = QStringLiteral("%1 - %2")
                        .arg(from.toString(QStringLiteral("hh:mm AP")),
                             to.toString(QStringLiteral("hh:mm AP")));
    if (minutesOfDay(from) > minutesOfDay(to))
        shape += QStringLiteral(" (overnight)");

    return QStringLiteral("%1: %2").arg(label, shape);
}

bool maskContainsDay(quint8 mask, int qtDayOfWeek)
{
    // Qt: Monday == 1 ... Sunday == 7. Our mask: Monday == bit 0.
    return (mask >> (qtDayOfWeek - 1)) & 0x1;
}

quint8 maskFromQtDay(int qtDayOfWeek)
{
    return quint8(1u << (qtDayOfWeek - 1));
}

bool ruleMatches(const Rule &rule, const QDateTime &when)
{
    if (!rule.enabled)
        return false;

    const int from = minutesOfDay(parseClock(rule.from));
    const int to = minutesOfDay(parseClock(rule.to));
    if (from < 0 || to < 0)
        return false;  // malformed times never match

    const int nowMinutes = when.time().hour() * 60 + when.time().minute();
    if (!windowContains(from, to, nowMinutes))
        return false;

    // Which day the window BELONGS to, which is not always today.
    //
    // An overnight window is one span of time that crosses midnight, so its after-midnight
    // tail is part of the previous day's occurrence. Checking the mask against today meant a
    // Monday-only 22:00 -> 07:00 window stopped dead at midnight and never limited Tuesday's
    // early hours - the window looked configured and simply did not apply. The tail now
    // counts against the day the window started on.
    //
    // Windows that do not cross midnight are unaffected, and so is the every-day case: a
    // daily mask contains both days, so it matches either way.
    int dayOffset = 0;
    if (from > to && nowMinutes < to)
        dayOffset = -1;

    const QDate owner = when.date().addDays(dayOffset);
    return maskContainsDay(rule.days, owner.dayOfWeek());
}

EffectiveLimits evaluate(const Settings &settings, const QDateTime &now)
{
    EffectiveLimits result;

    if (!settings.enabled) {
        result.throttling = false;
        result.reason = QStringLiteral("Limiting is switched off");
        return result;
    }

    for (int i = 0; i < settings.rules.size(); ++i) {
        const Rule &rule = settings.rules.at(i);
        if (!ruleMatches(rule, now))
            continue;

        result.down = rule.down;
        result.up = rule.up;
        result.throttling = (rule.down > 0 || rule.up > 0);
        result.ruleIndex = i;
        result.reason = describeRule(rule, i);
        if (!result.throttling)
            result.reason += QStringLiteral(" (uncapped)");
        return result;
    }

    result.down = settings.defaultDown;
    result.up = settings.defaultUp;
    result.throttling = (settings.defaultDown > 0 || settings.defaultUp > 0);
    result.reason = settings.rules.isEmpty()
                        ? QStringLiteral("Default limits")
                        : QStringLiteral("Default limits (no schedule active)");
    return result;
}

} // namespace net
