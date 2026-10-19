// Verifies UsageTracker accounting and persistence.
//
// The tracker is the one subsystem whose data must survive restarts (it drives
// the 30-day panel), so this exercises: accumulation, day attribution, the
// monthly JSON round-trip, cycle arithmetic, and reset semantics.
//
// Build/run: tools/run_tests.ps1
#include "../src/usagetracker.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

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

static void checkNear(double actual, double expected, double tolerance, const QString &what)
{
    check(std::fabs(actual - expected) <= tolerance,
          QStringLiteral("%1 (got %2, expected %3 +/- %4)")
              .arg(what)
              .arg(actual, 0, 'f', 2)
              .arg(expected, 0, 'f', 2)
              .arg(tolerance, 0, 'f', 2));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString scratch = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("usage_test_scratch"));
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    const double MB = 1024.0 * 1024.0;
    const double GB = 1024.0 * MB;

    // -- fresh tracker accumulates samples ---------------------------------
    {
        UsageTracker t;
        t.setDataDirectory(scratch);
        UsageConfig cfg;
        cfg.quotaBytes = 500 * GB;
        cfg.cycle = CycleKind::Rolling30;
        cfg.cycleStart = QDate::currentDate().addDays(-11);  // 12-day-old cycle
        t.load(cfg);

        checkNear(t.cycleTotalBytes(), 0.0, 0.001, "fresh tracker starts at zero");

        t.addSample(100 * MB, 20 * MB);
        t.addSample(50 * MB, 10 * MB);

        checkNear(t.cycleDownBytes(), 150 * MB, 0.001, "download accumulates");
        checkNear(t.cycleUpBytes(), 30 * MB, 0.001, "upload accumulates");
        checkNear(t.cycleTotalBytes(), 180 * MB, 0.001, "total is down + up");

        // -- cycle arithmetic ----------------------------------------------
        // Start was 11 days ago, Rolling30 => 29 days after start => 18 to go.
        check(t.daysUntilReset(cfg) == 18,
              QStringLiteral("daysUntilReset for an 11-day-old rolling-30 cycle (got %1)")
                  .arg(t.daysUntilReset(cfg)));

        const QDate expectedEnd = cfg.cycleStart.addDays(29);
        check(t.cycleEnd(cfg) == expectedEnd, "cycleEnd is start + 29 days");

        // Day attribution: everything landed on today's bucket.
        const auto days = t.cycleDays(cfg);
        check(days.size() == 12, QStringLiteral("cycleDays covers 12 days (got %1)")
                                     .arg(days.size()));
        if (!days.isEmpty()) {
            const auto &today = days.last();
            check(today.date == QDate::currentDate(), "last bucket is today");
            checkNear(today.down, 150 * MB, 0.001, "today's download bucket");
        }

        // -- reset clears the cycle ----------------------------------------
        t.resetCurrentCycle(cfg);
        checkNear(t.cycleTotalBytes(), 0.0, 0.001, "reset zeroes the cycle total");

        // -- persistence round-trip ----------------------------------------
        t.addSample(42 * MB, 7 * MB);
        t.flush();

        UsageTracker reloaded;
        reloaded.setDataDirectory(scratch);
        reloaded.load(cfg);

        checkNear(reloaded.cycleTotalBytes(), 49 * MB, 0.001,
                  "usage survives a reload");
        checkNear(reloaded.cycleDownBytes(), 42 * MB, 0.001, "reloaded download total");
        checkNear(reloaded.cycleUpBytes(), 7 * MB, 0.001, "reloaded upload total");
    }

    // -- the JSON file is actually well-formed ------------------------------
    {
        const QString month = QDate::currentDate().toString(QStringLiteral("yyyy-MM"));
        const QString path = QStringLiteral("%1/usage-%2.json").arg(scratch, month);
        check(QFile::exists(path), QStringLiteral("usage file written: %1").arg(path));

        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
            check(err.error == QJsonParseError::NoError, "usage JSON parses cleanly");
            check(doc.isObject() && doc.object().contains(QStringLiteral("days")),
                  "usage JSON has a days array");
        }
    }

    // -- Monthly cycle uses calendar months, not 30 days --------------------
    {
        UsageConfig cfg;
        cfg.cycle = CycleKind::Monthly;
        cfg.cycleStart = QDate(2026, 1, 15);

        UsageTracker t;
        t.setDataDirectory(scratch);
        t.load(cfg);

        // 15 Jan + 1 month - 1 day = 14 Feb.
        check(t.cycleEnd(cfg) == QDate(2026, 2, 14),
              QStringLiteral("monthly cycle ends 14 Feb (got %1)")
                  .arg(t.cycleEnd(cfg).toString(Qt::ISODate)));
    }

    // -- corrupt history must not crash or lose the session -----------------
    {
        const QString badDir = scratch + QStringLiteral("/corrupt");
        QDir().mkpath(badDir);
        const QString month = QDate::currentDate().toString(QStringLiteral("yyyy-MM"));
        QFile bad(QStringLiteral("%1/usage-%2.json").arg(badDir, month));
        if (bad.open(QIODevice::WriteOnly)) {
            bad.write("{ this is not valid json ");
            bad.close();
        }

        UsageTracker t;
        t.setDataDirectory(badDir);
        UsageConfig cfg;
        cfg.cycleStart = QDate::currentDate().addDays(-2);
        t.load(cfg);

        checkNear(t.cycleTotalBytes(), 0.0, 0.001, "corrupt file yields zero, not garbage");

        t.addSample(5 * MB, 0);
        checkNear(t.cycleTotalBytes(), 5 * MB, 0.001, "tracker still works after corruption");
        t.flush();
    }

    QDir(scratch).removeRecursively();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
