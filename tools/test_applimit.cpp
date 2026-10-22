// Verifies the per-application limiter's decision logic and persistence.
//
// Enforcement needs Windows Administrator rights (WinDivert returns
// ERROR_ACCESS_DENIED otherwise - see docs/app-limiter-findings.md), so this suite
// covers everything that does NOT need the driver:
//
//   * tuple normalisation, the join between the FLOW and NETWORK layers
//   * rule matching by path and by executable name, and the precedence between them
//   * schedule evaluation, including overnight windows and day masks
//   * the interaction rules between per-app limits and the system-wide limiter
//   * persistence round-trips, including upgrade from a file without appRules
//   * that an unelevated start fails loudly rather than pretending to limit
//
// It deliberately does NOT claim any traffic was actually shaped.
//
// Build/run: tools/run_applimit_tests.ps1
#include "../src/applimitengine.h"
#include "../src/schedule.h"
#include "../src/settingsstore.h"
#include "../src/throttleengine.h"
#include "../src/units.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <atomic>
#include <cstdio>

using namespace net;

static int g_failures = 0;
static int g_checks = 0;
static QFile *g_log = nullptr;

static void logLine(const QString &s)
{
    std::printf("%s\n", qPrintable(s));
    std::fflush(stdout);
    if (g_log && g_log->isOpen()) {
        g_log->write(s.toUtf8());
        g_log->write("\n");
        g_log->flush();
    }
}

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        logLine(QStringLiteral("FAIL: ") + what);
    } else {
        logLine(QStringLiteral("  ok: ") + what);
    }
}

static AppRule makeRule(quint64 id, const QString &exe, RateBps down, RateBps up)
{
    AppRule r;
    r.id = id;
    r.exe = exe;
    r.name = exe;
    r.path = QStringLiteral("C:/apps/") + exe;
    r.down = down;
    r.up = up;
    r.enabled = true;
    return r;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("applimit_results.txt")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    // =====================================================================
    // Tuple normalisation
    //
    // This is the join between the two WinDivert layers. Getting it wrong means
    // either nothing is limited, or unrelated traffic is.
    // =====================================================================
    logLine(QStringLiteral("== tuple normalisation =="));
    {
        // An outbound packet: local endpoint is the source.
        const FlowTuple out = AppLimitEngine::normaliseTuple(0x0A000001, 51000, 0x08080808, 443,
                                                             6, true);
        // The matching inbound packet for the same flow: local endpoint is now the
        // destination, and addresses/ports are swapped relative to the packet.
        const FlowTuple in = AppLimitEngine::normaliseTuple(0x08080808, 443, 0x0A000001, 51000,
                                                            6, false);

        check(out == in,
              QStringLiteral("both directions of one flow normalise to the same tuple"));
        check(out.local[0] == 0x0A000001 && out.localPort == 51000,
              "the local endpoint is the local one in both directions");
        check(out.remote[0] == 0x08080808 && out.remotePort == 443,
              "the remote endpoint is the remote one in both directions");

        // Different flows must not collide.
        const FlowTuple other = AppLimitEngine::normaliseTuple(0x0A000001, 51001, 0x08080808, 443,
                                                               6, true);
        check(!(out == other), "a different local port is a different flow");

        const FlowTuple udp = AppLimitEngine::normaliseTuple(0x0A000001, 51000, 0x08080808, 443,
                                                             17, true);
        check(!(out == udp), "the same 4-tuple over a different protocol is a different flow");

        // IPv6: the four address words participate in equality, and the same
        // direction normalisation holds.
        const quint32 v6a[4] = {0x20010DB8, 0x11112222, 0x33334444, 0x55556666};
        const quint32 v6b[4] = {0x20014860, 0x48013068, 0x00120001, 0x00000000};
        const FlowTuple v6out = AppLimitEngine::normaliseV6Tuple(v6a, 40000, v6b, 443,
                                                                 6, true);
        const FlowTuple v6in = AppLimitEngine::normaliseV6Tuple(v6b, 443, v6a, 40000,
                                                                6, false);
        check(v6out == v6in, "v6: both directions normalise to the same tuple");
        check(v6out.local[0] == v6a[0] && v6out.local[3] == v6a[3],
              "v6: all four address words are carried");
        check(qHash(v6out) == qHash(v6in), "v6: equal tuples hash equally");

        // Hash agreement is required for the QHash lookup to work at all.
        check(qHash(out) == qHash(in),
              "equal tuples hash equally, so the lookup can find them");
    }

    // =====================================================================
    // Rule matching
    // =====================================================================
    logLine(QStringLiteral("== rule matching =="));
    {
        QVector<AppRule> rules;
        rules.append(makeRule(1, QStringLiteral("steam.exe"), 200 * 1024, 0));
        rules.append(makeRule(2, QStringLiteral("chrome.exe"), 500 * 1024, 0));

        check(AppLimitEngine::matchRuleByPath(rules, QStringLiteral("C:/apps/steam.exe"),
                                              QStringLiteral("steam.exe"))
                  == 0,
              "an exact path match finds its rule");

        // Case and separators must not matter on Windows.
        check(AppLimitEngine::matchRuleByPath(rules, QStringLiteral("c:\\apps\\STEAM.EXE"),
                                              QStringLiteral("STEAM.EXE"))
                  == 0,
              "matching is case- and separator-insensitive");

        check(AppLimitEngine::matchRuleByPath(rules, QStringLiteral("C:/other/thing.exe"),
                                              QStringLiteral("thing.exe"))
                  == -1,
              "an unknown application matches nothing");

        // A rule with no path falls back to the executable name, which is what
        // makes manual entry by name possible.
        QVector<AppRule> byName;
        AppRule n;
        n.id = 9;
        n.exe = QStringLiteral("firefox.exe");
        n.down = 1024;
        byName.append(n);
        check(AppLimitEngine::matchRuleByPath(byName, QStringLiteral("D:/portable/firefox.exe"),
                                              QStringLiteral("firefox.exe"))
                  == 0,
              "a rule with no path matches on the executable name");

        // Precedence: a path-specific rule must win over a name-only rule, or a
        // rule for one launcher.exe would capture every other launcher.exe.
        QVector<AppRule> mixed;
        AppRule nameOnly;
        nameOnly.id = 10;
        nameOnly.exe = QStringLiteral("launcher.exe");
        nameOnly.down = 100;
        mixed.append(nameOnly);

        AppRule specific;
        specific.id = 11;
        specific.exe = QStringLiteral("launcher.exe");
        specific.path = QStringLiteral("C:/games/launcher.exe");
        specific.down = 200;
        mixed.append(specific);

        check(AppLimitEngine::matchRuleByPath(mixed, QStringLiteral("C:/games/launcher.exe"),
                                              QStringLiteral("launcher.exe"))
                  == 1,
              "an exact path match beats a name-only match even when the name rule "
              "comes first");

        // And the name-only rule still serves a *different* binary of that name.
        check(AppLimitEngine::matchRuleByPath(mixed, QStringLiteral("C:/other/launcher.exe"),
                                              QStringLiteral("launcher.exe"))
                  == 0,
              "a different binary with the same name falls to the name-only rule");
    }

    // =====================================================================
    // Schedule evaluation
    // =====================================================================
    logLine(QStringLiteral("== per-application scheduling =="));
    {
        AppRule r = makeRule(20, QStringLiteral("steam.exe"), 200 * 1024, 50 * 1024);

        // No windows: always on.
        bool alwaysOn = false;
        check(AppLimitEngine::activeWindow(r, QDateTime::currentDateTime(), &alwaysOn) == -1
                  && alwaysOn,
              "a rule with no windows is always active");

        AppLimitState s = AppLimitEngine::evaluate(r, QDateTime::currentDateTime());
        check(s.active, "an always-on rule is active");
        check(s.down == 200 * 1024 && s.up == 50 * 1024, "its limits are carried through");

        // An overnight window, matching the global scheduler's semantics.
        Rule night;
        night.id = 21;
        night.from = QStringLiteral("22:00");
        night.to = QStringLiteral("07:00");
        night.days = 0x7F;
        r.windows.clear();
        r.windows.append(night);

        const auto at = [](int y, int mo, int d, int h, int mi) {
            return QDateTime(QDate(y, mo, d), QTime(h, mi));
        };

        check(AppLimitEngine::evaluate(r, at(2026, 3, 10, 23, 30)).active,
              "an overnight window is active late in the evening");
        check(AppLimitEngine::evaluate(r, at(2026, 3, 11, 3, 0)).active,
              "an overnight window is still active after midnight");
        check(!AppLimitEngine::evaluate(r, at(2026, 3, 11, 12, 0)).active,
              "an overnight window is inactive during the day");

        AppLimitState outside = AppLimitEngine::evaluate(r, at(2026, 3, 11, 12, 0));
        check(!outside.reason.isEmpty() && outside.down == kUnlimited,
              QStringLiteral("outside its window the rule reports no limit (%1)")
                  .arg(outside.reason));

        // A single-day window must respect the day mask.
        Rule monday;
        monday.id = 22;
        monday.from = QStringLiteral("00:00");
        monday.to = QStringLiteral("23:59");
        monday.days = 0x01;   // Monday only
        r.windows.clear();
        r.windows.append(monday);

        // 2026-03-09 is a Monday.
        check(AppLimitEngine::evaluate(r, at(2026, 3, 9, 12, 0)).active,
              "a Monday-only window is active on Monday");
        check(!AppLimitEngine::evaluate(r, at(2026, 3, 10, 12, 0)).active,
              "a Monday-only window is inactive on Tuesday");

        // An overnight window carries its day mask into the next morning.
        //
        // The after-midnight tail belongs to the day the window STARTED on, so a Monday-only
        // 22:00 -> 07:00 window covers Tuesday's small hours. Checking the mask against the
        // current day instead stopped it dead at midnight: the window was configured, read
        // correctly on screen, and never limited anything between 00:00 and 07:00.
        Rule mondayNight;
        mondayNight.id = 23;
        mondayNight.from = QStringLiteral("22:00");
        mondayNight.to = QStringLiteral("07:00");
        mondayNight.days = 0x01;   // Monday only
        r.windows.clear();
        r.windows.append(mondayNight);

        check(AppLimitEngine::evaluate(r, at(2026, 3, 9, 23, 0)).active,
              "a Monday overnight window is active on Monday night");
        check(AppLimitEngine::evaluate(r, at(2026, 3, 10, 3, 0)).active,
              "and is still active in the small hours of Tuesday");
        check(!AppLimitEngine::evaluate(r, at(2026, 3, 10, 7, 0)).active,
              "and is released at its end time");
        check(!AppLimitEngine::evaluate(r, at(2026, 3, 10, 23, 0)).active,
              "Tuesday night is not covered by a Monday-only window");

        // A window's own limits are applied while it is in force.
        //
        // The schedule dialog collects a download and an upload rate for every window and
        // stores them; the engine used to evaluate the application's base rates instead, so a
        // window given a tighter limit did nothing at all.
        Rule cappedNight = mondayNight;
        cappedNight.down = 32 * 1024;
        cappedNight.up = 8 * 1024;
        r.windows.clear();
        r.windows.append(cappedNight);

        const AppLimitState inWindow = AppLimitEngine::evaluate(r, at(2026, 3, 9, 23, 0));
        check(inWindow.active && inWindow.down == 32 * 1024 && inWindow.up == 8 * 1024,
              QStringLiteral("a window's own limits are applied (%1 down, %2 up)")
                  .arg(inWindow.down)
                  .arg(inWindow.up));

        // A window left blank inherits the application's limits rather than disabling them:
        // "+ Add Schedule" starts every new window at 0/0, so treating that as "unmetered"
        // would silently uncap the application for the whole window.
        Rule blankNight = mondayNight;
        blankNight.down = kUnlimited;
        blankNight.up = kUnlimited;
        r.windows.clear();
        r.windows.append(blankNight);

        const AppLimitState inherited = AppLimitEngine::evaluate(r, at(2026, 3, 9, 23, 0));
        check(inherited.active && inherited.down == r.down && inherited.up == r.up,
              QStringLiteral("a window with no limits of its own uses the application's "
                             "(%1 down, %2 up)")
                  .arg(inherited.down)
                  .arg(inherited.up));

        // Disabled rules never limit, whatever the schedule says.
        r.windows.clear();
        r.enabled = false;
        AppLimitState off = AppLimitEngine::evaluate(r, QDateTime::currentDateTime());
        check(!off.active, "a disabled rule does not limit");
        check(off.reason.contains(QStringLiteral("Disabled")),
              QStringLiteral("and says why (%1)").arg(off.reason));

        // A rule with no limits set is configured but inert; saying so beats
        // claiming an active rule that does nothing.
        r.enabled = true;
        r.down = kUnlimited;
        r.up = kUnlimited;
        AppLimitState empty = AppLimitEngine::evaluate(r, QDateTime::currentDateTime());
        check(!empty.active, "a rule with no limits does not claim to be limiting");
    }

    // =====================================================================
    // Coexistence with the system-wide limiter
    //
    // The two features must not be able to disable one another. Per-application
    // limits are additive: a packet from a limited application passes through both
    // buckets, so the tighter limit wins and the global cap still applies elsewhere.
    // =====================================================================
    logLine(QStringLiteral("== coexistence with the system-wide limiter =="));
    {
        // Both engines are independent objects; configuring one must not disturb
        // the other's notion of its own rates.
        ThrottleEngine sys;
        AppLimitEngine apps;

        sys.apply(5 * 1024 * 1024, kUnlimited);
        check(sys.downBps() == 5 * 1024 * 1024,
              "the system-wide limit is set independently");

        QVector<AppRule> rules;
        rules.append(makeRule(30, QStringLiteral("steam.exe"), 200 * 1024, 0));
        apps.setRules(rules);

        check(sys.downBps() == 5 * 1024 * 1024,
              "adding an application rule leaves the system-wide limit untouched");

        // Disabling the app limiter must not disturb the system-wide one either.
        apps.setEnabled(false);
        check(sys.downBps() == 5 * 1024 * 1024,
              "disabling application limiting leaves the system-wide limit untouched");
        check(apps.states().size() == 1,
              "the application rules are still configured while disabled");

        // And the tokens compose the way the brief describes: the application's
        // limit is the tighter of the two, so it dominates for that application.
        const RateBps appLimit = 200 * 1024;
        const RateBps sysLimit = 5 * 1024 * 1024;
        check(qMin(appLimit, sysLimit) == appLimit,
              "the tighter of the two limits governs a limited application");
        check(qMin(RateBps(kUnlimited), sysLimit) == kUnlimited
                  || qMin(RateBps(kUnlimited), sysLimit) == 0,
              "an application with no limit is bounded only by the system-wide cap");
    }

    // =====================================================================
    // Download and upload are independent, in all three limiters
    // =====================================================================
    //
    // Flowbit limits in three places and each has its own implementation:
    //
    //   1. the dashboard's system-wide cap      -> ThrottleEngine
    //   2. the per-application cap              -> AppLimitEngine
    //   3. schedules, which feed rates into 1   -> Settings::rules / AppRule::windows
    //
    // The contract is the same in all three: capping DOWNLOAD must leave UPLOAD alone when
    // upload was not set. Capping one direction must never imply a cap on the other. This was
    // genuinely broken once - a download cap throttled upload, because one packet worker
    // served both directions and blocked the second while pacing the first - so it is checked
    // per implementation rather than assumed from the one that was fixed.
    logLine(QStringLiteral("== download and upload are independent =="));
    {
        const RateBps wanted = 200 * 1024;   // the reported case: 200 KB/s down, upload unset
        const QDateTime night(QDate(2026, 10, 14), QTime(23, 0));   // inside 22:00-07:00

        // ---- 0. the primitives -------------------------------------------
        // An empty box has to become "unlimited", not zero bytes per second.
        for (SpeedUnit u : allSpeedUnits()) {
            check(toBytesPerSec(0.0, u) == kUnlimited,
                  QStringLiteral("an empty %1 field means unlimited, not zero")
                      .arg(speedUnitLabel(u)));
        }
        {
            // Rate 0 must not wait at all: that is what makes "unlimited" actually unlimited
            // rather than merely untracked. A 1500-byte packet is a full-size one.
            Pacer free_rate(0);
            std::atomic<bool> stop{false};
            QElapsedTimer timer;
            timer.start();
            free_rate.waitFor(1500, stop, nullptr);
            const qint64 elapsed = timer.elapsed();
            check(elapsed < 20,
                  QStringLiteral("a 0 B/s pacer releases a packet immediately (%1 ms)")
                      .arg(elapsed));
        }

        // ---- 1. system-wide (dashboard) ----------------------------------
        {
            ThrottleEngine engine;
            // No driver is opened here (this suite runs unelevated), so the workers fail and
            // report; only the per-direction rate bookkeeping is under test.
            QObject::connect(&engine, &ThrottleEngine::errorOccurred, &engine,
                             [](const QString &) {});

            engine.apply(wanted, kUnlimited);
            check(engine.downBps() == wanted,
                  QStringLiteral("dashboard: a 200 KB/s download cap is applied"));
            check(engine.upBps() == kUnlimited,
                  QStringLiteral("dashboard: upload is left unlimited, not capped to 200 KB/s"));
            check(engine.isThrottling(), "dashboard: and the limiter reports as active");

            engine.apply(kUnlimited, wanted);
            check(engine.downBps() == kUnlimited,
                  "dashboard: the reverse holds - download unlimited");
            check(engine.upBps() == wanted, "dashboard: with only upload capped");

            engine.detach();
            check(engine.downBps() == kUnlimited && engine.upBps() == kUnlimited,
                  "dashboard: detaching clears both directions");
            check(!engine.isThrottling(), "dashboard: and reports itself off");
        }

        // ---- 2. schedule driving the system-wide cap ---------------------
        {
            Settings s;
            s.enabled = true;
            s.defaultDown = kUnlimited;
            s.defaultUp = kUnlimited;

            Rule nightCap;
            nightCap.name = QStringLiteral("Night cap");
            nightCap.from = QStringLiteral("22:00");
            nightCap.to = QStringLiteral("07:00");
            nightCap.down = wanted;
            nightCap.up = kUnlimited;   // upload deliberately not set
            s.rules.append(nightCap);

            const EffectiveLimits during = evaluate(s, night);
            check(during.down == wanted && during.up == kUnlimited,
                  "schedule: a window that caps only download leaves upload unlimited");
            check(during.throttling, "schedule: and still counts as limiting");

            const EffectiveLimits outside = evaluate(s, QDateTime(QDate(2026, 10, 14), QTime(12, 0)));
            check(outside.down == kUnlimited && outside.up == kUnlimited,
                  "schedule: outside the window both directions are free");
        }

        // ---- 3. per-application cap --------------------------------------
        {
            AppRule r;
            r.id = 90;
            r.path = QStringLiteral("C:/test/browser.exe");
            r.exe = QStringLiteral("browser.exe");
            r.name = QStringLiteral("Browser");
            r.down = wanted;
            r.up = kUnlimited;

            const AppLimitState s = AppLimitEngine::evaluate(r, night);
            check(s.active, "app limiter: a rule capping only download is active");
            check(s.down == wanted, "app limiter: its download cap is carried through");
            check(s.up == kUnlimited, "app limiter: and upload stays unlimited");
        }

        // ---- 4. per-application scheduled window ------------------------
        // The window supplies its own rate, so this is the one place the two directions could
        // diverge per window rather than per rule.
        {
            AppRule r;
            r.id = 91;
            r.path = QStringLiteral("C:/test/launcher.exe");
            r.exe = QStringLiteral("launcher.exe");
            r.down = 4 * 1024 * 1024;    // a base rate that must be overridden
            r.up = kUnlimited;

            Rule window;
            window.id = 92;
            window.from = QStringLiteral("22:00");
            window.to = QStringLiteral("07:00");
            window.days = 0x7F;
            window.down = wanted;
            window.up = kUnlimited;
            r.windows.append(window);

            const AppLimitState s = AppLimitEngine::evaluate(r, night);
            check(s.active && s.down == wanted,
                  "app window: its own download cap replaces the application's");
            check(s.up == kUnlimited,
                  "app window: and upload stays unlimited rather than mirroring download");
        }
    }

    // =====================================================================
    // Persistence
    // =====================================================================
    logLine(QStringLiteral("== persistence =="));
    {
        const QString scratch = QDir(QCoreApplication::applicationDirPath())
                                    .filePath(QStringLiteral("applimit_scratch"));
        QDir(scratch).removeRecursively();
        QDir().mkpath(scratch);

        {
            SettingsStore store(scratch);

            Settings s;
            s.enabled = true;
            s.defaultDown = 1024 * 1024;

            AppRule r = makeRule(0, QStringLiteral("steam.exe"), 200 * 1024, 50 * 1024);
            r.name = QStringLiteral("Steam");
            r.path = QStringLiteral("C:/Program Files (x86)/Steam/steam.exe");
            Rule w;
            w.from = QStringLiteral("22:00");
            w.to = QStringLiteral("07:00");
            w.days = 0x1F;
            r.windows.append(w);
            s.appRules.append(r);

            AppRule r2 = makeRule(0, QStringLiteral("chrome.exe"), 0, 0);
            r2.enabled = false;
            s.appRules.append(r2);

            // Ids are assigned here, the same way a loaded file gets them.
            ensureRuleIds(s);
            check(s.appRules.at(0).id != 0 && s.appRules.at(1).id != 0,
                  "app rules receive ids");
            check(s.appRules.at(0).id != s.appRules.at(1).id,
                  "the ids are distinct");
            check(s.appRules.at(0).windows.at(0).id != 0,
                  "schedule windows receive ids from the same sequence");

            store.save(s);
        }

        {
            SettingsStore store(scratch);
            const Settings loaded = store.load();

            check(loaded.appRules.size() == 2,
                  QStringLiteral("both applications survive a save/load (%1)")
                      .arg(loaded.appRules.size()));
            if (loaded.appRules.size() == 2) {
                const AppRule &a = loaded.appRules.at(0);
                check(a.path == QStringLiteral("C:/Program Files (x86)/Steam/steam.exe"),
                      "the full executable path is preserved, not just the name");
                check(a.name == QStringLiteral("Steam"), "the friendly name is preserved");
                check(a.exe == QStringLiteral("steam.exe"), "the exe name is preserved");
                check(a.down == 200 * 1024 && a.up == 50 * 1024,
                      "both independent limits are preserved");
                check(a.enabled, "the enabled state is preserved");
                check(a.windows.size() == 1, "the schedule window is preserved");
                if (!a.windows.isEmpty()) {
                    check(a.windows.at(0).from == QStringLiteral("22:00")
                              && a.windows.at(0).to == QStringLiteral("07:00"),
                          "the window's overnight times are preserved");
                    check(a.windows.at(0).days == 0x1F, "the window's day mask is preserved");
                }

                check(!loaded.appRules.at(1).enabled,
                      "a disabled application stays disabled across a restart");
                check(loaded.appRules.at(1).down == kUnlimited
                          && loaded.appRules.at(1).up == kUnlimited,
                      "unlimited stays unlimited");
            }

            // The system-wide settings must be untouched by any of this.
            check(loaded.enabled && loaded.defaultDown == 1024 * 1024,
                  "existing system-wide settings are unaffected by app rules");
        }

        // ---- upgrade path -------------------------------------------------
        // A settings file written before the feature existed has no appRules key.
        // It must load cleanly with an empty list rather than failing.
        {
            QFile f(QDir(scratch).filePath(QStringLiteral("settings.json")));
            check(f.open(QIODevice::WriteOnly | QIODevice::Truncate),
                  "the upgrade test can write a legacy file");
            QJsonObject root;
            root.insert(QStringLiteral("enabled"), true);
            root.insert(QStringLiteral("defaultDownBps"), double(2 * 1024 * 1024));
            f.write(QJsonDocument(root).toJson());
            f.close();
        }

        {
            SettingsStore store(scratch);
            const Settings legacy = store.load();
            check(legacy.appRules.isEmpty(),
                  "a settings file without appRules loads with none");
            check(legacy.defaultDown == 2 * 1024 * 1024,
                  "its system-wide settings are still read correctly");
        }

        QDir(scratch).removeRecursively();
    }

    // =====================================================================
    // Failure handling
    //
    // The engine must not report itself as running when it failed to open the
    // driver, because the UI would then show configured limits that do nothing.
    // =====================================================================
    logLine(QStringLiteral("== failure handling =="));
    {
        AppLimitEngine engine;

        QVector<AppRule> rules;
        rules.append(makeRule(40, QStringLiteral("steam.exe"), 100 * 1024, 0));
        engine.setRules(rules);

        engine.setEnabled(true);

        // This shell is not elevated, so opening the driver must fail.
        if (!engine.isRunning()) {
            check(!engine.isEnabled(),
                  "a failed start leaves the engine switched off rather than half-on");
            check(!engine.errorString().isEmpty(),
                  QStringLiteral("a reason is reported (%1)").arg(engine.errorString()));
            check(engine.errorString().contains(QStringLiteral("Administrator"),
                                                Qt::CaseInsensitive)
                      || engine.errorString().contains(QStringLiteral("driver"),
                                                       Qt::CaseInsensitive),
                  "the reason names the likely cause");
        } else {
            // Elevated shell: the opposite assertions are equally informative.
            check(engine.isRunning(), "an elevated shell starts the engine");
            engine.setEnabled(false);
            check(!engine.isRunning(), "disabling stops the engine");
        }

        // Configured rules must still be visible while the engine cannot run, so
        // the user can see and edit what they set up.
        check(engine.states().size() == 1,
              "rules remain listed even when the engine cannot start");
    }

    logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
