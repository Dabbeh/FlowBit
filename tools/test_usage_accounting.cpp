#include "usagetracker.h"
#include "units.h"
#include <QCoreApplication>
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
        // cumulative counters, as the monitor reports them
        t.addAppSample("C:/x/steam.exe", "Steam", 100, 10);
        t.addAppSample("C:/x/steam.exe", "Steam", 150, 30);
        t.addAppSample("C:/x/steam.exe", "Steam", 160, 35);
        const auto apps = t.appUsageOverDays(1);
        quint64 down = 0, up = 0;
        for (const auto &a : apps) if (a.path == "C:/x/steam.exe") { down = a.down; up = a.up; }
        std::printf("  after cumulative 100,150,160 -> down=%llu up=%llu\n",
                    (unsigned long long)down, (unsigned long long)up);
        check(down == 160, "cumulative samples are stored as deltas (160, not 410)");
        check(up == 35, "and the same for upload (35, not 75)");
        check(down + up == 195, "total equals the last cumulative pair");
    }
    {
        UsageTracker t; t.setDataDirectory(dir.path() + "/b");
        t.addAppSample("C:/x/a.exe", "A", 1000, 0);
        t.addAppSample("C:/x/a.exe", "A", 20, 0);      // counter restarted
        const auto apps = t.appUsageOverDays(1);
        quint64 down = 0;
        for (const auto &a : apps) if (a.path == "C:/x/a.exe") down = a.down;
        std::printf("  after restart 1000 -> 20 : down=%llu\n", (unsigned long long)down);
        check(down == 1020, "a counter reset counts the new value, never wraps to 2^64");
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
        check(down == 100, "an impossible single sample is refused");
    }
    std::printf("\n  formatBytes(139 TB) = %s\n", qPrintable(formatBytes(139.0 * 1024 * 1024 * 1024 * 1024)));
    check(formatBytes(139.0 * 1024 * 1024 * 1024 * 1024).contains("TB"),
          "139 TB is labelled TB, not 142751.4 GB");
    check(formatBytes(2.8 * 1024 * 1024 * 1024).contains("GB"), "2.8 GB still reads GB");
    check(formatBytes(102.2 * 1024 * 1024).contains("MB"), "102.2 MB still reads MB");
    std::printf("\n%d checks, %d failures\n%s\n", checks, fails, fails == 0 ? "usage_accounting passed." : "FAILED.");
    return fails == 0 ? 0 : 1;
}

