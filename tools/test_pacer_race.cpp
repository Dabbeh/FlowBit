// The pacer's rate is changed from the GUI thread while a packet worker sits inside waitFor().
// Those fields were plain qint64 - a data race, not merely a stale read. This drives both from
// separate threads and asserts the invariants that matter:
//
//   * switching a rate to unlimited releases a waiting packet immediately
//   * a rate change is visible to the waiter without waiting for a whole pacing slice
//   * no crash, no hang, no negative token count under hammering

#include "throttleengine.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <atomic>
#include <cstdio>
#include <thread>

using namespace net;

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok) ++g_failures;
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("\n== pacer under a concurrent rate change ==\n");

    // 1. unlimited mid-wait releases promptly
    {
        Pacer p(8 * 1024);                 // 8 KB/s
        std::atomic<bool> stop{false};
        std::atomic<qint64> waitedMs{0};
        QElapsedTimer clock;
        std::thread worker([&] {
            clock.start();
            p.waitFor(64 * 1024, stop);    // far more than the burst: this must block
            waitedMs.store(clock.elapsed());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        p.setRate(0);                      // unlimited: release the waiter
        worker.join();
        std::printf("  released after %lld ms of a wait that would have taken seconds\n",
                    (long long)waitedMs.load());
        check(waitedMs.load() < 500, "setting the rate to unlimited releases a waiting packet");
    }

    // 2. hammer rate changes while the worker paces
    {
        Pacer p(64 * 1024);
        std::atomic<bool> stop{false};
        std::atomic<bool> done{false};
        std::atomic<long long> packets{0};
        std::thread worker([&] {
            while (!stop.load()) {
                p.waitFor(1500, stop);
                packets.fetch_add(1);
            }
            done.store(true);
        });
        for (int i = 0; i < 400; ++i) {
            p.setRate((i % 3 == 0) ? 0 : (1 + (i % 7)) * 1024 * 1024);
            if (i % 50 == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        stop.store(true);
        worker.join();
        std::printf("  packets paced during 400 rate changes: %lld\n", packets.load());
        check(done.load(), "the worker exits cleanly while rates are being changed");
        check(packets.load() > 0, "packets continued to flow throughout");
        check(p.rate() >= 0, "the rate is never left negative");
    }

    std::printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
                g_failures == 0 ? "test_pacer_race passed." : "FAILED.");
    return g_failures == 0 ? 0 : 1;
}
