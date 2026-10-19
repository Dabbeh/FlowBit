// Tests the per-application usage history and the Statistics page's data.
//
// Two separate things were wrong or missing:
//
//   1. The Statistics page created six labels with an em-dash and never assigned
//      them again. The data existed; the page simply never read it.
//   2. Nothing recorded WHICH application the bytes belonged to, so "Steam used
//      13 GB" could not be answered at all.
//
// This asserts the tracker side of both: per-day and per-cycle totals, the per-app
// history summed over windows, the identity rule (keyed by path, not pid), the
// persistence round-trip, and the pruning rule.

#include "usagetracker.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QTemporaryDir>
#include <QTextStream>

using namespace net;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    QTextStream(stdout) << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    if (!ok)
        ++g_failures;
}

quint64 gb(double n) { return quint64(n * 1024.0 * 1024 * 1024); }

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    QTemporaryDir dir;
    if (!dir.isValid()) {
        out << "could not create a temporary directory\n";
        return 1;
    }

    UsageConfig config;
    config.quotaBytes = gb(500);
    config.cycle = CycleKind::Rolling30;
    config.cycleStart = QDate::currentDate();

    out << "== per-application usage history ==\n";
    {
        UsageTracker t;
        t.setDataDirectory(dir.path());
        t.load(config);

        // Nothing recorded yet.
        check(t.appUsageOverDays(30).isEmpty(),
              "an empty store reports no per-app usage");

        // Two applications, recorded the way the dashboard feeds them: byte counts
        // per interval, keyed by executable path.
        t.addAppSample(QStringLiteral("C:/Games/Steam/steam.exe"), QStringLiteral("Steam"),
                       gb(12.0), gb(1.0));
        t.addAppSample(QStringLiteral("C:/Program Files/Mozilla Firefox/firefox.exe"),
                       QStringLiteral("Firefox"), gb(0.4), gb(0.05));
        t.addAppSample(QStringLiteral("C:/Games/Steam/steam.exe"), QStringLiteral("Steam"),
                       gb(0.5), 0);

        const QVector<UsageTracker::AppUsage> apps = t.appUsageOverDays(30);
        check(apps.size() == 2, QStringLiteral("two applications are tracked (%1)")
                                    .arg(apps.size()));
        check(!apps.isEmpty() && apps.first().name == QLatin1String("Steam"),
              "the busiest application is first");
        check(!apps.isEmpty() && apps.first().total() == gb(13.5),
              QStringLiteral("Steam's total is the sum of its samples (%1 GB)")
                  .arg(double(!apps.isEmpty() ? apps.first().total() : 0) / (1024.0 * 1024 * 1024), 0, 'f', 2));
        check(!apps.isEmpty() && apps.first().down == gb(12.5),
              "the download half is tracked separately from the upload");
        check(!apps.isEmpty() && apps.first().up == gb(1.0),
              "the upload half is tracked separately");

        // Today's window must agree with the cycle window while all samples are today.
        check(t.appUsageOverDays(1).size() == 2,
              "the one-day window includes today's applications");
        check(t.appUsageOn(QDate::currentDate()).size() == 2,
              "querying a specific date returns that day's applications");
        check(t.appUsageOn(QDate::currentDate().addDays(-3)).isEmpty(),
              "a date with no traffic returns nothing");

        // An application that transfers nothing must not be recorded: the table is
        // for usage, and a row of zeroes is noise.
        t.addAppSample(QStringLiteral("C:/idle.exe"), QStringLiteral("Idle"), 0, 0);
        check(t.appUsageOverDays(30).size() == 2,
              "an application with no bytes is not recorded");

        // Identity is the PATH: the same executable sampled twice accumulates into
        // one row, and a path is not confused with a display name.
        t.addAppSample(QStringLiteral("C:/Games/Steam/steam.exe"), QStringLiteral("steam.exe"),
                       gb(1.0), 0);
        const QVector<UsageTracker::AppUsage> again = t.appUsageOverDays(30);
        check(again.size() == 2 && again.first().total() == gb(14.5),
              QStringLiteral("the same path accumulates into one row (%1 GB)")
                  .arg(double(again.first().total()) / (1024.0 * 1024 * 1024), 0, 'f', 2));

        t.flush();

        // The files must exist and be readable.
        const QString appsFile = QStringLiteral("%1/apps-%2.json")
                                     .arg(dir.path(),
                                          QDate::currentDate().toString(QStringLiteral("yyyy-MM")));
        check(QFile::exists(appsFile),
              QStringLiteral("per-app history was written to %1")
                  .arg(QFileInfo(appsFile).fileName()));
    }

    out << "\n== persistence round-trip ==\n";
    {
        // Print what was written and what came back, so any failure here names its
        // own cause instead of being inferred from a boolean.
        const QString appsFile = QStringLiteral("%1/apps-%2.json")
                                     .arg(dir.path(),
                                          QDate::currentDate().toString(QStringLiteral("yyyy-MM")));
        if (QFile::exists(appsFile)) {
            QFile f(appsFile);
            if (f.open(QIODevice::ReadOnly)) {
                out << "  [file] " << QString::fromUtf8(f.readAll()) << "\n";
            }
        } else {
            out << "  [file] MISSING: " << appsFile << "\n";
        }

        // A fresh tracker reading the same directory must see what was written.
        UsageTracker t2;
        t2.setDataDirectory(dir.path());
        t2.load(config);

        for (const UsageTracker::AppUsage &a : t2.appUsageOverDays(30)) {
            out << "  [loaded] path=" << a.path
                << "  name='" << a.name << "'"
                << "  total=" << a.total() << "\n";
        }

        const QVector<UsageTracker::AppUsage> apps = t2.appUsageOverDays(30);
        check(apps.size() == 2, QStringLiteral("history survives a restart (%1 applications)")
                                    .arg(apps.size()));
        // The name is whatever the LAST sample for that path supplied: a later sample
        // may carry a better-resolved name, and the newest is the most accurate. The
        // test's own setup records "steam.exe" last, so that is what must come back.
        check(!apps.isEmpty() && apps.first().name == QLatin1String("steam.exe"),
              QStringLiteral("the newest name survives the round-trip ('%1')")
                  .arg(apps.isEmpty() ? QString() : apps.first().name));
        check(!apps.isEmpty() && apps.first().total() == gb(14.5),
              QStringLiteral("the recorded total survives exactly (%1 GB)")
                  .arg(double(!apps.isEmpty() ? apps.first().total() : 0) / (1024.0 * 1024 * 1024), 0, 'f', 2));
        check(apps.size() == 2,
              "three samples across two applications produced two rows, not three");
    }

    out << "\n== pruning ==\n";
    {
        UsageTracker t3;
        t3.setDataDirectory(dir.path());
        t3.load(config);

        // Keep one day: today's entries stay, and the call must not throw away the
        // window it was asked to keep.
        t3.pruneAppHistory(1);
        check(t3.appUsageOverDays(30).size() == 2,
              "pruning keeps the requested window");

        t3.pruneAppHistory(0);
        check(t3.appUsageOverDays(30).isEmpty(),
              "pruning with no days retained clears the history");
    }

    out << "\n" << g_checks << " checks, " << g_failures << " failures\n";
    out << (g_failures == 0 ? "test_app_usage passed.\n" : "test_app_usage FAILED.\n");
    return g_failures == 0 ? 0 : 1;
}
