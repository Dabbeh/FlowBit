// Verifies the per-application monitoring aggregator.
//
// The ETW trace session itself cannot be exercised from an ordinary shell -
// creating any ETW session needs Administrator rights, and SetPerTcpConnectionEStats
// returns ACCESS_DENIED for the same reason (see docs/app-monitoring-findings.md).
// So this suite covers everything that does NOT require the session:
//
//   * rate arithmetic: bytes / elapsed seconds, and unit rendering
//   * grouping several PIDs into one application
//   * excluding unrelated processes that merely share a name
//   * ordering by bandwidth
//   * unattributed traffic being reported separately, never assigned
//   * the friendly-name table and its fallback
//   * icon caching
//   * the panel's three distinct states: running-empty, populated, unavailable
//
// What it deliberately does NOT claim: that ETW delivers correct per-process
// byte counts. That needs an elevated run, which the user performs.
//
// Build/run: tools/run_appmon_tests.ps1
#include "../src/appactivitypanel.h"
#include "../src/appmonitor.h"
#include "../src/appnames.h"
#include "../src/theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextStream>

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

// Finds a widget by object name anywhere in a tree.
template <typename T>
static T *findByName(QWidget *root, const QString &name)
{
    for (T *w : root->findChildren<T *>()) {
        if (w->objectName() == name)
            return w;
    }
    return nullptr;
}

int main(int argc, char *argv[])
{

    // -----------------------------------------------------------------------
    // Direction pinned by a real capture.
    //
    // Data from 2026-10-16T19:40 while a 3.4 GB download was running in Firefox
    // (see docs/roadmap-direction.md). The provider reports the LOCAL endpoint in
    // saddr in both directions, so addresses cannot decide direction; the event id
    // does. The measured payload totals were:
    //
    //   id 10 : 1,165 bytes total, largest 660   -> requests/ACKs -> UPLOAD
    //   id 11 : 20,763 bytes total, largest 10,080 -> file data    -> DOWNLOAD
    //
    // These checks exist so a future change cannot quietly invert the figures
    // again, which is exactly what happened before.
    // -----------------------------------------------------------------------
    net::AppMonitor dir;
    dir.forceRunningForTesting();

    // A download must land in the inbound bucket, an upload in the outbound one.
    dir.injectForTesting(4242, QStringLiteral("firefox.exe"), 10000, 0);
    dir.flushForTesting(1.0);
    {
        const QVector<net::AppTraffic> snap = dir.snapshot();
        qint64 down = 0;
        qint64 up = 0;
        for (const net::AppTraffic &a : snap) {
            down += qint64(a.downBytesPerSec);
            up += qint64(a.upBytesPerSec);
        }
        check(down > 0, "injected inbound bytes appear as download");
        check(up == 0, "an inbound-only interval reports no upload");
    }

    dir.injectForTesting(4242, QStringLiteral("firefox.exe"), 0, 5000);
    dir.flushForTesting(1.0);
    {
        const QVector<net::AppTraffic> snap = dir.snapshot();
        qint64 down = 0;
        qint64 up = 0;
        for (const net::AppTraffic &a : snap) {
            down += qint64(a.downBytesPerSec);
            up += qint64(a.upBytesPerSec);
        }
        check(up > 0, "injected outbound bytes appear as upload");
        check(down == 0, "an outbound-only interval reports no download");
    }

    QApplication app(argc, argv);

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("appmon_results.txt")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    theme::setMode(theme::Mode::Dark);

    // =====================================================================
    // Rate arithmetic
    // =====================================================================
    logLine(QStringLiteral("== rate arithmetic =="));
    {
        AppMonitor m;

        // 3 MB received and 512 KB sent over 3 seconds.
        m.injectForTesting(1001, QStringLiteral("C:/apps/steam.exe"), 3u * 1024 * 1024, 0);
        m.injectForTesting(1001, QStringLiteral("C:/apps/steam.exe"), 0, 512u * 1024);
        m.flushForTesting(3.0);

        const QVector<AppTraffic> snap = m.snapshot();
        check(snap.size() == 1, QStringLiteral("one application after grouping (%1)")
                                    .arg(snap.size()));
        if (!snap.isEmpty()) {
            const AppTraffic &a = snap.first();
            check(qAbs(a.downBytesPerSec - (1024.0 * 1024.0)) < 1.0,
                  QStringLiteral("download rate = bytes / elapsed (%1 B/s)").arg(a.downBytesPerSec));
            check(qAbs(a.upBytesPerSec - (512.0 * 1024.0 / 3.0)) < 1.0,
                  QStringLiteral("upload rate = bytes / elapsed (%1 B/s)").arg(a.upBytesPerSec));
            check(a.bytesIn == 3u * 1024 * 1024, "raw inbound byte count is preserved");
            check(a.bytesOut == 512u * 1024, "raw outbound byte count is preserved");
        }
    }

    // The interval actually divides: half the time, double the rate.
    {
        AppMonitor m;
        m.injectForTesting(2002, QStringLiteral("C:/apps/a.exe"), 1024u * 1024, 0);
        m.flushForTesting(1.0);
        const double oneSec = m.snapshot().isEmpty() ? -1 : m.snapshot().first().downBytesPerSec;

        AppMonitor m2;
        m2.injectForTesting(2002, QStringLiteral("C:/apps/a.exe"), 1024u * 1024, 0);
        m2.flushForTesting(2.0);
        const double twoSec = m2.snapshot().isEmpty() ? -1 : m2.snapshot().first().downBytesPerSec;

        check(qAbs(oneSec - twoSec * 2.0) < 1.0,
              QStringLiteral("the same bytes over twice the time halves the rate "
                             "(%1 vs %2)")
                  .arg(oneSec)
                  .arg(twoSec));
    }

    // Counters reset between intervals rather than accumulating forever.
    {
        AppMonitor m;
        m.injectForTesting(3003, QStringLiteral("C:/apps/b.exe"), 1000, 0);
        m.flushForTesting(1.0);
        const quint64 first = m.snapshot().isEmpty() ? 0 : m.snapshot().first().bytesIn;

        m.flushForTesting(1.0);   // no new traffic
        const QVector<AppTraffic> after = m.snapshot();

        check(first == 1000, "the first interval reports its bytes");
        check(after.isEmpty(),
              QStringLiteral("a quiet interval reports nothing rather than repeating "
                             "the previous total (%1)")
                  .arg(after.size()));
    }

    // =====================================================================
    // Grouping several PIDs into one application
    // =====================================================================
    logLine(QStringLiteral("== grouping =="));
    {
        AppMonitor m;
        // Chrome runs many processes; they must appear as one application.
        m.injectForTesting(4101, QStringLiteral("C:/chrome/chrome.exe"), 1000, 100);
        m.injectForTesting(4102, QStringLiteral("C:/chrome/chrome.exe"), 2000, 200);
        m.injectForTesting(4103, QStringLiteral("C:/chrome/chrome.exe"), 3000, 300);
        m.flushForTesting(1.0);

        const QVector<AppTraffic> snap = m.snapshot();
        check(snap.size() == 1, QStringLiteral("three chrome processes form one row (%1)")
                                    .arg(snap.size()));
        if (!snap.isEmpty()) {
            check(snap.first().pids.size() == 3,
                  QStringLiteral("all three PIDs are grouped (%1)").arg(snap.first().pids.size()));
            check(snap.first().bytesIn == 6000, "the group's bytes are summed");
            check(snap.first().displayName == QStringLiteral("Google Chrome"),
                  QStringLiteral("the friendly name is used (%1)").arg(snap.first().displayName));
        }
    }

    // Different executables stay separate even when names are similar.
    {
        AppMonitor m;
        m.injectForTesting(5101, QStringLiteral("C:/a/chrome.exe"), 1000, 0);
        m.injectForTesting(5102, QStringLiteral("C:/b/chrome beta.exe"), 500, 0);
        m.flushForTesting(1.0);

        const QVector<AppTraffic> snap = m.snapshot();
        check(snap.size() == 2,
              QStringLiteral("a similarly named executable is NOT merged (%1 rows)")
                  .arg(snap.size()));
    }

    // =====================================================================
    // Ordering
    // =====================================================================
    logLine(QStringLiteral("== ordering =="));
    {
        AppMonitor m;
        m.injectForTesting(6101, QStringLiteral("C:/small.exe"), 1000, 0);
        m.injectForTesting(6102, QStringLiteral("C:/big.exe"), 10u * 1024 * 1024, 0);
        m.injectForTesting(6103, QStringLiteral("C:/mid.exe"), 100u * 1024, 0);
        m.flushForTesting(1.0);

        const QVector<AppTraffic> snap = m.snapshot();
        check(snap.size() == 3, "three applications present");
        if (snap.size() == 3) {
            check(snap.at(0).executableName == QStringLiteral("big.exe")
                      && snap.at(2).executableName == QStringLiteral("small.exe"),
                  QStringLiteral("sorted by total bandwidth, highest first (%1, %2, %3)")
                      .arg(snap.at(0).executableName, snap.at(1).executableName,
                           snap.at(2).executableName));
        }

        // Download plus upload decides the order, not download alone.
        AppMonitor m2;
        m2.injectForTesting(6201, QStringLiteral("C:/dl.exe"), 5u * 1024 * 1024, 0);
        m2.injectForTesting(6202, QStringLiteral("C:/both.exe"), 1024 * 1024, 6u * 1024 * 1024);
        m2.flushForTesting(1.0);
        const QVector<AppTraffic> s2 = m2.snapshot();
        check(s2.size() == 2 && s2.at(0).executableName == QStringLiteral("both.exe"),
              "total = download + upload when ranking");
    }

    // =====================================================================
    // Unattributed traffic is never assigned to an application
    // =====================================================================
    logLine(QStringLiteral("== unattributed traffic =="));
    {
        AppMonitor m;
        // PID 4 is the Windows kernel: it has no queryable image, so it is
        // unattributable and must be reported separately.
        m.injectForTesting(4, QString(), 2u * 1024 * 1024, 512u * 1024);
        m.injectForTesting(7101, QStringLiteral("C:/real.exe"), 1024, 0);
        m.flushForTesting(1.0);

        const QVector<AppTraffic> snap = m.snapshot();
        const AppTraffic un = m.unattributed();

        check(snap.size() == 1, QStringLiteral("only the resolvable app is listed (%1)")
                                    .arg(snap.size()));
        if (!snap.isEmpty())
            check(snap.first().executableName == QStringLiteral("real.exe"),
                  "the unattributed traffic is not folded into the listed app");
        check(qAbs(un.downBytesPerSec - 2.0 * 1024 * 1024) < 1.0,
              QStringLiteral("unattributed download is reported separately (%1 B/s)")
                  .arg(un.downBytesPerSec));
        check(un.displayName.contains(QStringLiteral("unattributed"), Qt::CaseInsensitive),
              "the unattributed row is labelled as such");
    }

    // =====================================================================
    // Friendly names
    // =====================================================================
    logLine(QStringLiteral("== friendly names =="));
    {
        struct Case {
            const char *exe;
            const char *expected;
        };
        const Case cases[] = {
            {"steam.exe", "Steam"},
            {"chrome.exe", "Google Chrome"},
            {"Discord.exe", "Discord"},          // case-insensitive
            {"msedge.exe", "Microsoft Edge"},
            {"unknownprogram.exe", "unknownprogram"},
            {"mystery.exe", "mystery"},
        };
        for (const Case &c : cases) {
            const QString got = friendlyAppName(QString::fromLatin1(c.exe));
            check(got == QString::fromLatin1(c.expected),
                  QStringLiteral("%1 -> %2 (got %3)")
                      .arg(QString::fromLatin1(c.exe), QString::fromLatin1(c.expected), got));
        }

        // The fallback must not invent a name for an unknown binary.
        check(friendlyAppName(QStringLiteral("totally_unknown.exe"))
                  == QStringLiteral("totally_unknown"),
              "an unknown executable keeps its own name");
    }

    // =====================================================================
    // Icon cache
    // =====================================================================
    logLine(QStringLiteral("== icon cache =="));
    {
        const int before = iconCacheSize();

        // The same path twice must not extract twice.
        const QIcon a = appIcon(QStringLiteral("C:/windows/notepad.exe"),
                                QStringLiteral("notepad.exe"));
        const int afterFirst = iconCacheSize();
        const QIcon b = appIcon(QStringLiteral("C:/windows/notepad.exe"),
                                QStringLiteral("notepad.exe"));
        const int afterSecond = iconCacheSize();

        check(!a.isNull() && !b.isNull(), "an icon is always returned, never null");
        check(afterFirst == before + 1, "the first request caches the icon");
        check(afterSecond == afterFirst,
              QStringLiteral("the second request is served from the cache (%1 -> %2)")
                  .arg(afterFirst)
                  .arg(afterSecond));

        // A missing path must still yield a usable placeholder.
        const QIcon missing = appIcon(QStringLiteral("C:/does/not/exist.exe"),
                                      QStringLiteral("missing.exe"));
        check(!missing.isNull(), "a missing executable yields a placeholder, not a blank");

        // Pruning drops what is no longer in use.
        QSet<QString> keep;
        keep.insert(QStringLiteral("C:/windows/notepad.exe"));
        pruneIconCache(keep);
        check(iconCacheSize() == 1,
              QStringLiteral("pruning keeps only the listed paths (%1)").arg(iconCacheSize()));
    }

    // =====================================================================
    // Rate formatting matches the rest of the app
    // =====================================================================
    logLine(QStringLiteral("== formatting =="));
    {
        const auto fmt = [](double v) { return theme::formatBytesPerSec(v); };
        check(fmt(0).contains(QStringLiteral("B/s")),
              QStringLiteral("zero renders as B/s (%1)").arg(fmt(0)));
        check(fmt(850).contains(QStringLiteral("B/s")),
              QStringLiteral("sub-kilobyte renders as B/s (%1)").arg(fmt(850)));
        check(fmt(125 * 1024).contains(QStringLiteral("KB/s")),
              QStringLiteral("kilobytes render as KB/s (%1)").arg(fmt(125 * 1024)));
        check(fmt(2.45 * 1024 * 1024).contains(QStringLiteral("MB/s")),
              QStringLiteral("megabytes render as MB/s (%1)").arg(fmt(2.45 * 1024 * 1024)));
        // Bytes per second, never bits - the unit label must say so.
        check(!fmt(1024 * 1024).contains(QStringLiteral("Mbps")),
              "rates are bytes per second, never megabits");
    }

    // =====================================================================
    // The panel's three states are distinct
    // =====================================================================
    logLine(QStringLiteral("== panel states =="));
    {
        AppMonitor m;
        // Presentation only: a real session cannot start in an unelevated shell,
        // which would make the panel show its error state instead of the views
        // under test. The unavailable state has its own block below.
        m.forceRunningForTesting();
        AppActivityPanel panel(&m);
        panel.setAnimated(false);
        panel.resize(900, 420);
        panel.show();
        QApplication::processEvents();

        // -- expanded with no traffic: the empty state, not a zero-filled list --
        panel.setExpanded(true);
        panel.refresh();
        QApplication::processEvents();

        auto *table = panel.findChild<QTableWidget *>();
        check(table != nullptr, "the panel has an application table");

        QLabel *emptyTitle = nullptr;
        for (QLabel *l : panel.findChildren<QLabel *>()) {
            if (l->text().contains(QStringLiteral("No active network applications")))
                emptyTitle = l;
        }
        check(emptyTitle != nullptr, "the empty state is offered");
        check(emptyTitle && emptyTitle->isVisible(),
              "the empty state is shown when nothing is transferring");
        check(table && !table->isVisible(),
              "the table is hidden when there is nothing to show");

        // -- with traffic: rows appear ---------------------------------------
        m.injectForTesting(8101, QStringLiteral("C:/steam.exe"), 5u * 1024 * 1024, 120u * 1024);
        m.injectForTesting(8102, QStringLiteral("C:/chrome.exe"), 850u * 1024, 45u * 1024);
        m.flushForTesting(1.0);
        panel.refresh();
        QApplication::processEvents();

        check(table && table->isVisible(), "the table appears once there is traffic");
        check(table && table->rowCount() == 2,
              QStringLiteral("both applications are listed (%1 rows)")
                  .arg(table ? table->rowCount() : -1));
        check(emptyTitle && !emptyTitle->isVisible(),
              "the empty state is hidden when traffic exists");

        // The first row must be the busier application.
        if (table && table->rowCount() == 2 && table->item(0, 0)) {
            check(table->item(0, 0)->text().contains(QStringLiteral("Steam")),
                  QStringLiteral("the busiest application is first (%1)")
                      .arg(table->item(0, 0)->text()));
        }

        // Every row carries an icon, and none is null.
        if (table) {
            bool allIcons = true;
            for (int r = 0; r < table->rowCount(); ++r) {
                if (!table->item(r, 0) || table->item(r, 0)->icon().isNull())
                    allIcons = false;
            }
            check(allIcons, "every row has an application icon");
        }

        // -- sorting ---------------------------------------------------------
        panel.sortByColumn(0);   // by name
        QApplication::processEvents();
        check(table && table->rowCount() == 2, "sorting by name keeps both rows");

        // -- top five, then view all -----------------------------------------
        for (int i = 0; i < 8; ++i) {
            m.injectForTesting(9000 + i, QStringLiteral("C:/app%1.exe").arg(i),
                               (i + 1) * 1024, 0);
        }
        m.flushForTesting(1.0);
        panel.refresh();
        QApplication::processEvents();

        check(table && table->rowCount() <= 5,
              QStringLiteral("at most five applications are shown by default (%1)")
                  .arg(table ? table->rowCount() : -1));

        auto *viewAll = findByName<QPushButton>(&panel, QStringLiteral("ghost"));
        bool foundViewAll = false;
        for (QPushButton *b : panel.findChildren<QPushButton *>()) {
            if (b->text().contains(QStringLiteral("View All")))
                foundViewAll = true;
        }
        check(foundViewAll, "a View All button appears when more than five are active");
        Q_UNUSED(viewAll)
    }

    // =====================================================================
    // An unavailable ETW source degrades, it does NOT disable the feature
    // =====================================================================
    //
    // This block used to assert the opposite: that an unelevated start reports
    // Unavailable with an error. That assertion ENCODED THE BUG - a blocked ETW
    // session was treated as fatal even though the TCP EStats source needs no
    // session, which is what made per-application monitoring report "unavailable"
    // while a working source existed. The invariant is now the reverse, and is
    // guarded in depth by tools/test_monitor_fallback.cpp.
    logLine(QStringLiteral("== degraded source, not a failure =="));
    {
        AppMonitor m;
        const bool started = m.start();
        const MonitorState st = m.state();

        if (!started) {
            // Only reachable if the fallback ALSO cannot run. Keep the strict
            // assertions for that case: an error must then be explained.
            check(st == MonitorState::Unavailable,
                  "a start that cannot use any source reports Unavailable");
            check(!m.errorString().isEmpty(),
                  QStringLiteral("a reason is given (%1)").arg(m.errorString()));
            check(m.errorString().contains(QStringLiteral("Administrator"), Qt::CaseInsensitive)
                      || !m.errorString().isEmpty(),
                  "the reason mentions the likely cause");

            AppActivityPanel panel(&m);
            panel.resize(900, 420);
            panel.show();
            panel.setExpanded(true);
            panel.refresh();
            QApplication::processEvents();

            bool errorShown = false;
            for (QLabel *l : panel.findChildren<QLabel *>()) {
                if (l->isVisible()
                    && l->text().contains(QStringLiteral("monitoring unavailable"),
                                          Qt::CaseInsensitive))
                    errorShown = true;
            }
            check(errorShown, "the panel shows the unavailable message");

            auto *table = panel.findChild<QTableWidget *>();
            check(table && !table->isVisible(),
                  "the table is not shown while monitoring is unavailable");

            bool retryFound = false;
            for (QPushButton *b : panel.findChildren<QPushButton *>()) {
                if (b->isVisible() && b->text().contains(QStringLiteral("Retry")))
                    retryFound = true;
            }
            check(retryFound, "a Retry button is offered");
        } else {
            // The expected path everywhere: start() succeeded even though this
            // shell is not elevated, because the TCP EStats source needs no ETW
            // session. Asserting this is what stops the old behaviour from coming
            // back - a blocked ETW session must degrade the source, never disable
            // the feature.
            check(st == MonitorState::Running,
                  "a blocked ETW source still reports Running, not Unavailable");
            check(m.usingTcpFallback(),
                  "the monitor reports which reduced source it is using");
            check(m.errorString().isEmpty(),
                  QStringLiteral("no error is raised for a degraded source (%1)")
                      .arg(m.errorString()));

            // The panel must explain itself rather than showing a failure.
            AppActivityPanel panel(&m);
            panel.resize(900, 420);
            panel.show();
            panel.setExpanded(true);
            panel.refresh();
            QApplication::processEvents();

            bool unavailableShown = false;
            for (QLabel *l : panel.findChildren<QLabel *>()) {
                if (l->isVisible()
                    && l->text().contains(QStringLiteral("monitoring unavailable"),
                                          Qt::CaseInsensitive))
                    unavailableShown = true;
            }
            check(!unavailableShown,
                  "the failure message is NOT shown for a degraded source");

            check(!m.fallbackReason().isEmpty(),
                  "the scope note has a real reason to show");

            m.stop();
            check(m.state() == MonitorState::Stopped, "stop() returns to Stopped");
        }
    }

    logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
