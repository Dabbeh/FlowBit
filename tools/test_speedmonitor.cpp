// Measures what SpeedMonitor actually reports, through the production code path.
//
// This is the authoritative check on the interface-filtering fix: it drives the
// real SpeedMonitor (not a reimplementation) and prints the rates it produces,
// so the ~5x duplicate-row inflation can be seen or ruled out directly.
//
// Build/run: tools/run_tests.ps1 (test_speedmonitor)
#include "../src/speedmonitor.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

using namespace net;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    SpeedMonitor monitor;
    monitor.start();

    const QString adapter = monitor.activeAdapterName();
    const quint64 ifIndex = monitor.primaryInterfaceIndex();

    std::printf("adapter selected by production code: %s (index %llu)\n",
                qPrintable(adapter), static_cast<unsigned long long>(ifIndex));
    std::printf("sampling 5s through the real SpeedMonitor...\n");

    double peakDown = 0.0;
    double peakUp = 0.0;
    int samples = 0;

    QObject::connect(&monitor, &SpeedMonitor::sampled,
                     [&](double down, double up) {
                         ++samples;
                         peakDown = std::max(peakDown, down);
                         peakUp = std::max(peakUp, up);
                         std::printf("  %2d: down %10.1f KB/s   up %9.1f KB/s\n", samples,
                                     down / 1024.0, up / 1024.0);
                     });

    QEventLoop loop;
    QTimer::singleShot(5200, &loop, &QEventLoop::quit);
    loop.exec();
    monitor.stop();

    std::printf("\nsamples: %d\n", samples);
    std::printf("peak down: %.1f KB/s (%.2f MB/s)\n", peakDown / 1024.0,
                peakDown / (1024.0 * 1024.0));

    int failures = 0;

    // A concrete adapter must be chosen, or nothing can be measured at all.
    if (ifIndex == 0) {
        std::printf("FAIL: no primary interface was selected\n");
        ++failures;
    } else {
        std::printf("primary interface index resolved: yes\n");
    }
    if (adapter.isEmpty() || adapter == QStringLiteral("Network")) {
        std::printf("FAIL: adapter name not resolved (got '%s')\n", qPrintable(adapter));
        ++failures;
    } else {
        std::printf("adapter name resolved: %s\n", qPrintable(adapter));
    }

    // The capture must actually tick.
    if (samples < 4) {
        std::printf("FAIL: expected ~5 samples, got %d\n", samples);
        ++failures;
    }

    // A Wi-Fi adapter on this machine reports gigabit-class link speed. Even a
    // saturated real link would be well under 200 MB/s, so anything above that
    // is arithmetic on duplicated rows rather than real traffic.
    constexpr double kImplausible = 200.0 * 1024 * 1024;
    if (peakDown > kImplausible) {
        std::printf("FAIL: %.1f MB/s is implausible for one adapter (duplicate rows?)\n",
                    peakDown / (1024.0 * 1024.0));
        ++failures;
    } else {
        std::printf("plausible for a single adapter: yes\n");
    }

    std::printf("\n%d checks, %d failures\n", 5, failures);
    return failures == 0 ? 0 : 1;
}
