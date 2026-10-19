// Self-test for per-application monitoring. RUN THIS ELEVATED.
//
// Creating an ETW trace session requires Administrator rights, so the monitoring
// backend cannot be exercised from an ordinary shell. This tool starts the real
// monitor, generates network traffic, and prints what it measured - which is the
// only way to confirm the backend works on a given machine.
//
// Usage (from an elevated prompt):
//     appmon_selftest.exe [seconds]
//
// It writes the same output to appmon_selftest.log beside the executable.
#include "../src/appmonitor.h"
#include "../src/appnames.h"
#include "../src/theme.h"

#include "appmonitor.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include <windows.h>
#include <winhttp.h>

#include <cstdio>

using namespace net;

namespace {

QFile *g_log = nullptr;

void out(const QString &s)
{
    std::printf("%s\n", qPrintable(s));
    std::fflush(stdout);
    if (g_log && g_log->isOpen()) {
        g_log->write(s.toUtf8());
        g_log->write("\n");
        g_log->flush();
    }
}

// Pulls a few megabytes over HTTPS so there is definitely traffic to attribute.
// Runs on a worker thread; failures are ignored because the point is only to
// create load.
DWORD WINAPI downloadWorker(LPVOID)
{
    for (int i = 0; i < 3; ++i) {
        HINTERNET session = WinHttpOpen(L"Flowbit-SelfTest/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            continue;

        HINTERNET connect = WinHttpConnect(session, L"speed.cloudflare.com",
                                           INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connect) {
            HINTERNET request =
                WinHttpOpenRequest(connect, L"GET", L"/__down?bytes=6000000", nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
            if (request) {
                if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                    && WinHttpReceiveResponse(request, nullptr)) {
                    DWORD available = 0;
                    BYTE buffer[16384];
                    DWORD read = 0;
                    do {
                        available = 0;
                        if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
                            break;
                        DWORD toRead = available < sizeof(buffer) ? available
                                                                  : DWORD(sizeof(buffer));
                        if (!WinHttpReadData(request, buffer, toRead, &read))
                            break;
                    } while (read > 0);
                }
                WinHttpCloseHandle(request);
            }
            WinHttpCloseHandle(connect);
        }
        WinHttpCloseHandle(session);
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Honour the session-cleanup flag BEFORE anything else.
    //
    // AppMonitor::startSession spawns <this executable> --clear-monitor-session
    // when it finds a leftover trace session. Without this branch the child would
    // re-enter the whole self-test, find the same leftover, and spawn another
    // child: an unbounded chain of processes, each holding a live ETW consumer,
    // which keeps the trace session pinned so it can never be released. That is
    // precisely the "leftover session ... could not be cleared" loop.
    if (app.arguments().contains(QStringLiteral("--clear-monitor-session"))) {
        net::AppMonitor::clearGhostSession();
        return 0;
    }

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("appmon_selftest.log")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    const int seconds = (argc > 1) ? QString::fromLatin1(argv[1]).toInt() : 12;
    const int runFor = qBound(5, seconds, 120);

    out(QStringLiteral("=== Flowbit per-application monitoring self-test ==="));
    out(QStringLiteral("elevated: %1")
            .arg(AppMonitor::isElevated() ? QStringLiteral("YES") : QStringLiteral("NO")));
    out(QStringLiteral(""));

    if (!AppMonitor::isElevated()) {
        out(QStringLiteral("This tool must run elevated: creating an ETW trace session"));
        out(QStringLiteral("requires Administrator rights."));
        out(QStringLiteral(""));
        out(QStringLiteral("Right-click the executable and choose 'Run as administrator',"));
        out(QStringLiteral("or run it from an elevated PowerShell window."));
        return 2;
    }

    AppMonitor monitor;
    const bool started = monitor.start();

    out(QStringLiteral("monitor.start() -> %1").arg(started ? QStringLiteral("true")
                                                            : QStringLiteral("false")));
    if (!started) {
        out(QStringLiteral("reason: %1").arg(monitor.errorString()));
        out(QStringLiteral(""));
        out(QStringLiteral("The backend could not start. Report the reason above."));
        return 1;
    }

    out(QStringLiteral("state: Running"));
    out(QStringLiteral(""));

    // Generate real traffic while sampling.
    HANDLE worker = CreateThread(nullptr, 0, downloadWorker, nullptr, 0, nullptr);
    out(QStringLiteral("generating download traffic for %1s...").arg(runFor));
    out(QStringLiteral(""));

    QElapsedTimer timer;
    timer.start();
    int interval = 0;
    int intervalsWithTraffic = 0;
    quint64 seenPrev = 0;
    quint64 totalBytesSeen = 0;

    while (timer.elapsed() < runFor * 1000) {
        // Let the trace session deliver events for one interval, then close it -
        // the self-test drives the interval clock itself, exactly like the app's
        // dashboard timer does.
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        loop.exec();
        monitor.tick();

        ++interval;
        if (int(monitor.eventsSeenTotal() - seenPrev) > 0 || interval % 5 == 0)
            out(QStringLiteral("        [etw] seen=%1 decoded=%2")
                    .arg(monitor.eventsSeenTotal()).arg(monitor.eventsDecodedTotal()));
        seenPrev = monitor.eventsSeenTotal();
        const QVector<AppTraffic> apps = monitor.snapshot();
        const AppTraffic un = monitor.unattributed();

        double intervalTotal = 0;
        for (const AppTraffic &a : apps)
            intervalTotal += a.totalBytesPerSec();
        intervalTotal += un.totalBytesPerSec();
        totalBytesSeen += quint64(intervalTotal);

        if (apps.isEmpty() && un.totalBytesPerSec() <= 0) {
            out(QStringLiteral("  [%1] no per-application traffic this second")
                    .arg(interval, 2, 10, QLatin1Char('0')));
            continue;
        }

        ++intervalsWithTraffic;
        out(QStringLiteral("  [%1] %2 application(s)")
                .arg(interval, 2, 10, QLatin1Char('0'))
                .arg(apps.size()));

        for (const AppTraffic &a : apps) {
            if (a.totalBytesPerSec() <= 0)
                continue;
            out(QStringLiteral("        %1  \u2193 %2  \u2191 %3   (%4 process)")
                    .arg(a.displayName.left(28), -28)
                    .arg(theme::formatBytesPerSec(a.downBytesPerSec), 12)
                    .arg(theme::formatBytesPerSec(a.upBytesPerSec), 12)
                    .arg(a.pids.size()));
        }
        if (un.totalBytesPerSec() > 0) {
            out(QStringLiteral("        %1  \u2193 %2  \u2191 %3")
                    .arg(QStringLiteral("(system / unattributed)"), -28)
                    .arg(theme::formatBytesPerSec(un.downBytesPerSec), 12)
                    .arg(theme::formatBytesPerSec(un.upBytesPerSec), 12));
        }
    }

    if (worker) {
        WaitForSingleObject(worker, 5000);
        CloseHandle(worker);
    }

    out(QStringLiteral(""));
    out(QStringLiteral("=== address samples (direction decisions) ==="));
    out(monitor.addressMissReport().isEmpty()
            ? QStringLiteral("  (every event had a decisive local/remote pair)")
            : monitor.addressMissReport());
    out(QStringLiteral(""));

    out(QStringLiteral("intervals sampled      : %1").arg(interval));
    out(QStringLiteral("intervals with traffic : %1").arg(intervalsWithTraffic));

    if (intervalsWithTraffic == 0) {
        out(QStringLiteral(""));
        out(QStringLiteral("NO TRAFFIC WAS ATTRIBUTED. The session started, but either"));
        out(QStringLiteral("no events arrived or none carried a resolvable PID."));
        out(QStringLiteral("Please report this output - it means the backend is not"));
        out(QStringLiteral("delivering usable data."));
        monitor.stop();
        return 1;
    }

    out(QStringLiteral(""));
    out(QStringLiteral("PASS - per-application traffic was measured. Compare the names"));
    out(QStringLiteral("and rates above with what you were actually running."));

    monitor.stop();
    out(QStringLiteral(""));
    out(QStringLiteral("monitor.stop() -> state Stopped: %1")
            .arg(monitor.state() == MonitorState::Stopped ? QStringLiteral("yes")
                                                          : QStringLiteral("NO")));
    return 0;
}
