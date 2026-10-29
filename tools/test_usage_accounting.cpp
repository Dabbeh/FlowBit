#include "usagetracker.h"
#include "units.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
using namespace net;
static int checks = 0, fails = 0;
static void check(bool ok, const QString &what) { ++checks; std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what)); if (!ok) ++fails; }
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    std::printf("\n== per-application usage accounting ==\n");
    {
        UsageTracker t; t.setDataDirectory(dir.path() + "/a");
        // interval bytes, as the app monitor feeds them
        t.addAppSample("C:/x/steam.exe", "Steam", 100, 10);
        t.addAppSample("C:/x/steam.exe", "Steam", 150, 30);
        t.addAppSample("C:/x/steam.exe", "Steam", 160, 35);
        const auto apps = t.appUsageOverDays(1);
        quint64 down = 0, up = 0;
        for (const auto &a : apps) if (a.path == "C:/x/steam.exe") { down = a.down; up = a.up; }
        std::printf("  after intervals 100,150,160 -> down=%llu up=%llu\n",
                    (unsigned long long)down, (unsigned long long)up);
        check(down == 410, "interval samples are summed (100+150+160)");
        check(up == 75, "and the same for upload (10+30+35)");
        check(down + up == 485, "total is the sum of the intervals");
    }
    {
        UsageTracker t; t.setDataDirectory(dir.path() + "/b");
        t.addAppSample("C:/x/a.exe", "A", 1000, 0);
        t.addAppSample("C:/x/a.exe", "A", 20, 0);
        const auto apps = t.appUsageOverDays(1);
        quint64 down = 0;
        for (const auto &a : apps) if (a.path == "C:/x/a.exe") down = a.down;
        std::printf("  after intervals 1000 and 20 : down=%llu\n", (unsigned long long)down);
        check(down == 1020, "a small interval after a large one is still counted");
        check(down < 100000, "and the result stays plausible");
    }
    {
        UsageTracker t; t.setDataDirectory(dir.path() + "/c");
        t.addAppSample("C:/x/b.exe", "B", 100, 0);
        t.addAppSample("C:/x/b.exe", "B", 8000000000000000000ull, 0);   // 8e18 jump
        const auto apps = t.appUsageOverDays(1);
        quint64 down = 0;
        for (const auto &a : apps) if (a.path == "C:/x/b.exe") down = a.down;
        std::printf("  after an 8e18 jump : down=%llu\n", (unsigned long long)down);
        check(down == 100, "an impossible single interval is refused");
    }
    {
        UsageTracker t; t.setDataDirectory(dir.path() + "/d");
        t.addSample(1024, 512);
        t.addAppSample("C:/x/reset.exe", "Reset", 1024, 512);
        t.flush();
        const QString usage = QStringLiteral("%1/usage-%2.json")
                                  .arg(dir.path() + "/d",
                                       QDate::currentDate().toString("yyyy-MM"));
        const QString apps = QStringLiteral("%1/apps-%2.json")
                                 .arg(dir.path() + "/d",
                                      QDate::currentDate().toString("yyyy-MM"));
        check(QFile::exists(usage) && QFile::exists(apps),
              "reset test: both history files were written");
        check(t.resetAllUsage() == 0, "resetAllUsage reports no deletion failures");
        check(t.cycleTotalBytes() == 0, "resetAllUsage clears the cycle total");
        check(t.appUsageOverDays(1).isEmpty(), "resetAllUsage clears the app history");
        check(!QFile::exists(usage) && !QFile::exists(apps),
              "resetAllUsage deletes the persisted history");
    }

    std::printf("\n  formatBytes(139 TB) = %s\n", qPrintable(formatBytes(139.0 * 1024 * 1024 * 1024 * 1024)));
    check(formatBytes(139.0 * 1024 * 1024 * 1024 * 1024).contains("TB"),
          "139 TB is labelled TB, not 142751.4 GB");
    check(formatBytes(2.8 * 1024 * 1024 * 1024).contains("GB"), "2.8 GB still reads GB");
    check(formatBytes(102.2 * 1024 * 1024).contains("MB"), "102.2 MB still reads MB");
    std::printf("\n%d checks, %d failures\n%s\n", checks, fails, fails == 0 ? "usage_accounting passed." : "FAILED.");
    return fails == 0 ? 0 : 1;
}
