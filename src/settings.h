#pragma once

#include <QDate>
#include <QString>
#include <QVector>

namespace net {

// Speed limits are stored internally as BYTES PER SECOND. 0 means "unlimited".
//
// The UI exposes MB/s, KB/s and B/s, but keeping one canonical unit means those
// dropdowns are real conversions rather than relabelled kibibytes.
using RateBps = qint64;

constexpr RateBps kUnlimited = 0;

// How the 30-day usage quota resets.
enum class CycleKind {
    Monthly,    // resets on the same day-of-month as the cycle start
    Rolling30,  // resets 30 days after the cycle start
};

// One scheduled window, e.g. 22:00 -> 07:00 at 1.5 MB/s / 500 KB/s.
//
// Times are local wall-clock "HH:mm" strings. A rule whose "from" is later than
// its "to" wraps across midnight (22:00 -> 07:00 covers the whole night).
// from == to is treated as "all day".
//
// `days` is a 7-bit mask, Monday = bit 0 ... Sunday = bit 6. The default covers
// every day, preserving the earlier every-day behaviour.
struct Rule {
    // Stable identity.
    //
    // Rules used to be addressed by their position in the settings list, so any
    // reorder or rebuild acted on the wrong rule, and the UI had no way to refer
    // to a rule that had moved. The id is assigned once, persisted, and never
    // reused. 0 means "not yet assigned" and is filled in when settings load.
    quint64 id = 0;

    QString name;
    QString from = QStringLiteral("22:00");
    QString to = QStringLiteral("07:00");
    RateBps down = kUnlimited;
    RateBps up = kUnlimited;
    quint8 days = 0x7F;
    bool enabled = true;
};

// Configuration for the 30-day data usage panel.
struct UsageConfig {
    double quotaBytes = 500.0 * 1024 * 1024 * 1024;  // 500 GB default
    CycleKind cycle = CycleKind::Rolling30;
    QDate cycleStart = QDate::currentDate();
};

// One bandwidth rule that applies to a single application.
//
// Shaped exactly like `Rule` above - same day mask, same overnight semantics, same
// times-as-strings representation - because it reuses the same evaluator. The only
// difference is that it carries application identity, and that a per-application
// limit is *additive* to the system-wide one rather than replacing it.
struct AppRule {
    // Stable identity, assigned the same way as Rule::id, and for the same reason:
    // the UI must be able to act on a specific application after the list moves.
    quint64 id = 0;

    // Identity of the application.
    //
    // The full path is authoritative and the executable name is a display
    // convenience. Matching is done on the path when it is known, because several
    // unrelated binaries can share a name such as "launcher.exe". The name is kept
    // so a rule can still be shown and matched if the file is moved.
    QString path;
    QString name;       // friendly name for display, e.g. "Steam"
    QString exe;        // file name, e.g. "steam.exe"

    RateBps down = kUnlimited;
    RateBps up = kUnlimited;
    bool enabled = true;

    // Per-application schedule. Empty means "always active while enabled".
    //
    // Stored as a list rather than a single window so an application can have
    // several windows (for instance a work-hours cap and a late-night cap), the
    // same way the global scheduler works.
    QVector<Rule> windows;
};

struct Settings {
    // Master switch: when false the engine is fully detached from the network.
    bool enabled = false;

    // Baseline limits, applied whenever no scheduled rule matches.
    RateBps defaultDown = kUnlimited;
    RateBps defaultUp = kUnlimited;

    // Scheduled rules, evaluated in order; the first match wins.
    QVector<Rule> rules;

    // Per-application limits. Only applications the user has explicitly added
    // appear here; Flowbit never enumerates installed software on its own.
    //
    // These compose with the system-wide limits above instead of replacing them:
    // a packet belonging to a limited application passes through both the
    // per-application bucket and the system-wide one, so the tighter limit wins
    // and the global cap stays enforced for everything else.
    QVector<AppRule> appRules;

    // 30-day usage tracking configuration.
    UsageConfig usage;

    // Live throughput readout in the UI.
    bool showThroughput = true;

    // Appearance. Stored as a small enum so the file stays readable:
    // "auto" follows the Windows apps-light/dark setting, "light"/"dark" force it.
    enum class ThemeMode { Auto, Light, Dark };
    ThemeMode themeMode = ThemeMode::Auto;

    // ---- startup and closing ----------------------------------------------

    // Launch Flowbit when Windows starts, minimised to the tray.
    //
    // Off by default. An application that adds itself to startup without being asked is
    // the behaviour most people mean when they call something annoying, and the only way
    // to be sure it is welcome is to leave it off until it is turned on.
    //
    // Implemented as a per-user registry entry rather than a Startup-folder shortcut:
    // Flowbit needs to run elevated to shape traffic, and a shortcut cannot carry that
    // reliably, while a registry entry can launch it through the ordinary elevation path.
    bool startWithWindows = false;

    // What the window's X button does.
    //
    // true  - hide to the notification area and keep shaping, which is what Flowbit has
    //         always done and therefore stays the default;
    // false - quit, releasing the driver and stopping every limit.
    //
    // Whichever is chosen, the tray icon follows it: an application that keeps running
    // with no visible way to reach it is worse than one that closed.
    bool closeToTray = true;

    // Look for a newer version when Flowbit starts, and offer it.
    //
    // Only reads a small file. Nothing is ever downloaded or installed without the user
    // agreeing to it first.
    bool checkForUpdates = true;

    // The version the user chose to skip, so a dismissed update is not offered again on
    // every launch. Empty means nothing has been skipped.
    QString skippedVersion;

    // Whether the "Flowbit keeps running in the notification area" hint has been shown.
    //
    // It was a function-local static, so it reset on every launch: anyone who closes the window
    // daily saw the same notification daily. One-time means once, so the flag is stored.
    bool trayHintShown = false;
};

// The limits that are in force right now, plus why.
struct EffectiveLimits {
    RateBps down = kUnlimited;
    RateBps up = kUnlimited;
    bool throttling = false;   // false => unlimited in both directions
    int ruleIndex = -1;        // index of the matching rule, or -1 for defaults
    QString reason;            // short human-readable explanation
};

} // namespace net
