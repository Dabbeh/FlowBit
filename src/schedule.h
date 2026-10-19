#pragma once

#include "settings.h"

#include <QDateTime>
#include <QTime>

namespace net {

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

// Parses the time formats a person actually types, case-insensitively and with
// surrounding spaces ignored:
//
//     21        21:00      21:30      2130
//     9         9:30       9pm        9:30 pm     9.30pm      12am / 12pm
//
// "12am" is midnight and "12pm" is noon. Anything else yields an INVALID QTime -
// callers must check isValid(), because an invalid time makes a schedule window
// never match, silently.
//
// This used to accept only "H:MM" and "HHMM": a bare "21" or "9pm" was rejected,
// which is why schedules appeared to do nothing.
QTime parseClock(const QString &text);

// Canonical storage form for a time: always "HH:MM", so persisted settings are
// stable regardless of which input style the user typed. Empty for an invalid
// time.
QString formatClock(const QTime &t);

// Minutes since midnight for a valid QTime, or -1.
int minutesOfDay(const QTime &t);

// True when `nowMinutes` falls inside the window [fromMinutes, toMinutes),
// handling windows that wrap across midnight. from == to means "all day".
bool windowContains(int fromMinutes, int toMinutes, int nowMinutes);

// Day-mask helpers. Bit 0 = Monday ... bit 6 = Sunday (Qt::Monday == 1).
bool maskContainsDay(quint8 mask, int qtDayOfWeek);
quint8 maskFromQtDay(int qtDayOfWeek);

// Human-readable form of a rule window, e.g. "Rule 1: 22:00-07:00 (overnight)".
QString describeRule(const Rule &rule, int index);

// True when `rule` is active at `when` - weekday mask and time window both match.
bool ruleMatches(const Rule &rule, const QDateTime &when);

// ---------------------------------------------------------------------------
// Rule identity
//
// Rules are addressed by a stable id rather than by list position, which is what
// made deletion act on the wrong rule. Settings written before ids existed come
// back with id == 0, so ensureRuleIds() assigns them and reports whether anything
// changed so the caller can persist.
// ---------------------------------------------------------------------------

// Id to use for a new rule. Monotonic, and always above every id already in use
// in the process, so an id is never reused after a deletion.
quint64 nextRuleId();

// Walks the rule list assigning ids where missing. Returns true when at least one
// rule changed, so the caller knows a save is needed.
bool ensureRuleIds(Settings &settings);

// Index of the rule with `id`, or -1. The only safe way to locate a rule for
// editing or deletion.
int ruleIndexById(const Settings &settings, quint64 id);

// Same lookup for per-application rules. Kept separate from ruleIndexById()
// because the two lists are independent, and an index into one is never
// meaningful in the other.
int appRuleIndexById(const Settings &settings, quint64 id);

// ---------------------------------------------------------------------------
// Schedule evaluation
// ---------------------------------------------------------------------------

// Works out which limits apply at `now`. A disabled rule is skipped; the first
// enabled rule matching `now` wins; otherwise the baseline defaults apply.
// When `settings.enabled` is false nothing is enforced at all.
EffectiveLimits evaluate(const Settings &settings, const QDateTime &now);

} // namespace net
