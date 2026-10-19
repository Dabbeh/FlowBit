// Verifies the production Pacer (src/throttleengine.h) converges on the
// configured rate. This drives the real token-bucket code used for pacing
// packets - it does not re-implement the maths.
//
// Build/run: tools/run_tests.ps1
#include "../src/throttleengine.h"

#include <QDebug>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace net;
using Clock = std::chrono::steady_clock;

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

// Feeds `totalBytes` through the pacer as fixed-size packets and returns the
// elapsed wall time in seconds.
static double pump(Pacer &pacer, qint64 totalBytes, size_t packetSize,
                   std::atomic<bool> &stop)
{
    const auto start = Clock::now();
    qint64 sent = 0;
    while (sent < totalBytes) {
        pacer.waitFor(packetSize, stop, nullptr);
        sent += static_cast<qint64>(packetSize);
    }
    return std::chrono::duration<double>(Clock::now() - start).count();
}

int main()
{
    std::atomic<bool> stop{false};

    // -- unlimited never blocks --------------------------------------------
    {
        Pacer pacer(0);
        const double secs = pump(pacer, 8 * 1024 * 1024, 1500, stop);
        check(secs < 1.0, QStringLiteral("unlimited pacer must not delay (%1s)").arg(secs));
        check(pacer.rate() == 0, "unlimited pacer reports rate 0");
    }

    // -- 1 MB/s over 4 MB should take ~4 s --------------------------------
    {
        const qint64 rate = 1024 * 1024;
        Pacer pacer(rate);
        const qint64 total = 4 * rate;
        const double secs = pump(pacer, total, 1500, stop);
        const double achieved = total / secs;
        const double error = (achieved - rate) / rate;
        std::printf("  1 MB/s target: achieved %.2f MB/s (%.1f%% error) in %.2fs\n",
                    achieved / (1024 * 1024), error * 100.0, secs);
        // Allow the burst bucket (64 KB) plus scheduler jitter, but nothing close
        // to "unlimited" (which would be 10-100x faster).
        check(std::abs(error) < 0.25,
              QStringLiteral("1 MB/s pacing within 25%% (got %1%)").arg(error * 100.0));
    }

    // -- slow rate: 100 KB/s over 500 KB should take ~5 s ------------------
    {
        const qint64 rate = 100 * 1024;
        Pacer pacer(rate);
        const qint64 total = 5 * rate;
        const double secs = pump(pacer, total, 1500, stop);
        const double achieved = total / secs;
        const double error = (achieved - rate) / rate;
        std::printf("  100 KB/s target: achieved %.1f KB/s (%.1f%% error) in %.2fs\n",
                    achieved / 1024.0, error * 100.0, secs);
        check(std::abs(error) < 0.30,
              QStringLiteral("100 KB/s pacing within 30%% (got %1%)").arg(error * 100.0));
    }

    // -- rate change mid-flight takes effect promptly ----------------------
    {
        Pacer pacer(1024 * 1024);
        // Drain ~1 MB, then drop to 128 KB/s and confirm the second phase is
        // much slower per byte than the first.
        pump(pacer, 1024 * 1024, 1500, stop);

        pacer.setRate(128 * 1024);
        check(pacer.rate() == 128 * 1024, "setRate updates the rate");

        const qint64 total2 = 512 * 1024;
        const double secs2 = pump(pacer, total2, 1500, stop);
        const double achieved2 = total2 / secs2;
        std::printf("  after change to 128 KB/s: achieved %.1f KB/s\n", achieved2 / 1024.0);
        check(achieved2 < 300 * 1024,
              QStringLiteral("rate change applies (got %1 KB/s)").arg(achieved2 / 1024.0));
    }

    // -- switching to unlimited mid-flight releases immediately ------------
    {
        Pacer pacer(64 * 1024);
        std::atomic<bool> localStop{false};
        pacer.waitFor(1500, localStop, nullptr);  // consume the initial burst

        pacer.setRate(0);
        const double secs = pump(pacer, 16 * 1024 * 1024, 1500, localStop);
        std::printf("  unlimited after limit: 16 MB in %.2fs\n", secs);
        check(secs < 1.0, QStringLiteral("reverting to unlimited is immediate (%1s)").arg(secs));
    }

    // -- stop flag aborts a long wait --------------------------------------
    {
        Pacer pacer(8 * 1024);  // very slow: 8 KB/s
        std::atomic<bool> localStop{false};

        std::thread raiser([&localStop]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            localStop.store(true);
        });

        const auto start = Clock::now();
        // Ask for more than the bucket holds; with the stop flag raised the wait
        // must end quickly instead of pacing out the full deficit.
        pacer.waitFor(64 * 1024, localStop, nullptr);
        const double secs = std::chrono::duration<double>(Clock::now() - start).count();
        raiser.join();

        std::printf("  stop flag released the wait after %.2fs\n", secs);
        check(secs < 1.0, QStringLiteral("stop flag aborts pacing (%1s)").arg(secs));
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
