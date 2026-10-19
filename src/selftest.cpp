#include "selftest.h"

#include "appmonitor.h"
#include "schedule.h"
#include "settings.h"
#include "settingsstore.h"
#include "throttleengine.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSysInfo>
#include "theme.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>

#include <windows.h>

namespace net {

namespace {

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

enum class Outcome { Pass, Fail, Skip, Info };

struct Report {
    QStringList lines;
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    void section(const QString &title)
    {
        lines << QString();
        lines << title;
        lines << QString(title.size(), QLatin1Char('-'));
    }

    void item(Outcome o, const QString &name, const QString &detail = QString())
    {
        const char *tag = "[--]";
        switch (o) {
        case Outcome::Pass: tag = "[OK]"; ++passed; break;
        case Outcome::Fail: tag = "[!!]"; ++failed; break;
        case Outcome::Skip: tag = "[SK]"; ++skipped; break;
        case Outcome::Info: tag = "[--]"; break;
        }
        lines << QStringLiteral("%1 %2").arg(QLatin1String(tag), name);
        if (!detail.isEmpty()) {
            const QStringList parts = detail.split(QLatin1Char('\n'));
            for (const QString &p : parts)
                lines << QStringLiteral("       ") + p;
        }
    }

    QString render() const { return lines.join(QLatin1Char('\n')); }
};

bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

// A trace session that cannot be reclaimed is the failure mode that wasted the
// most time previously, so it is checked first and explicitly.
void checkTraceSession(Report &r)
{
    r.section(QStringLiteral("ETW TRACE SESSION"));

    const bool leftover = AppMonitor::leftoverSessionExists();
    if (leftover) {
        r.item(Outcome::Info, QStringLiteral("A session named '%1' already exists")
                                   .arg(AppMonitor::sessionName()),
               QStringLiteral("This is expected while Flowbit is running. If Flowbit is "
                              "NOT running, it is a leftover from a killed run: the "
                              "monitoring checks below will report whether it can still "
                              "be used."));
    } else {
        r.item(Outcome::Pass, QStringLiteral("No leftover trace session"));
    }
}

// Starts monitoring, waits a bounded time for real traffic, and reports what was
// actually measured. This is the check that matters most: it exercises the whole
// ETW path end to end.
void checkMonitoring(Report &r, int seconds)
{
    r.section(QStringLiteral("PER-APPLICATION MONITORING"));

    // Measured source availability, before anything is started. This is what turns
    // a future failure into a named cause instead of an inferred one: the report
    // states which sources this machine actually offers.
    r.item(Outcome::Info, QStringLiteral("Data sources tested"),
           AppMonitor::dataSourceReport());

    AppMonitor monitor;
    if (!monitor.start()) {
        r.item(Outcome::Fail, QStringLiteral("Could not start monitoring"),
               monitor.errorString());
        r.item(Outcome::Skip, QStringLiteral("Per-application attribution"),
               QStringLiteral("Skipped because the session did not start."));
        return;
    }
    r.item(Outcome::Pass, QStringLiteral("Monitoring session started"));

    QElapsedTimer clock;
    clock.start();
    int sawTrafficIntervals = 0;
    quint64 eventsBefore = monitor.eventsSeenTotal();

    // Bounded: never longer than `seconds`, with a hard cap so a wedged kernel
    // object cannot hang the process.
    while (clock.elapsed() < qint64(seconds) * 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        QThread::msleep(200);
        monitor.tick();
        if (!monitor.snapshot().isEmpty())
            ++sawTrafficIntervals;
    }

    const quint64 seen = monitor.eventsSeenTotal();
    const quint64 decoded = monitor.eventsDecodedTotal();

    // Report which source produced the figures, and judge the FEATURE, not the
    // transport. Zero ETW events is a fact about ETW, not a failure of monitoring:
    // the EStats source can be carrying the measurement. Failing here on ETW alone
    // is how a working feature was repeatedly reported as broken.
    r.item(Outcome::Info, QStringLiteral("Source in use"),
           monitor.usingTcpFallback()
               ? QStringLiteral("TCP connection counters (EStats)\n%1")
                     .arg(monitor.fallbackReason())
               : QStringLiteral("Kernel network events (ETW)"));

    r.item(Outcome::Info,
           QStringLiteral("Kernel network events received"),
           QStringLiteral("received: %1    decoded: %2    received this run: %3")
               .arg(seen)
               .arg(decoded)
               .arg(seen - eventsBefore)
               + (seen == 0
                      ? QStringLiteral("\nNo ETW events arrived. If the source above says "
                                       "EStats, that is expected and the figures below are "
                                       "still real - they simply exclude UDP/QUIC.")
                      : QString()));

    if (seen > 0) {
        const double pct = decoded * 100.0 / double(seen);
        r.item(pct >= 95.0 ? Outcome::Pass : Outcome::Fail,
               QStringLiteral("Events decoded"),
               QStringLiteral("%1% of received events produced a byte count "
                              "(a low figure means the payload schema differs from the "
                              "decoder)").arg(QString::number(pct, 'f', 1)));
    }

    if (sawTrafficIntervals > 0) {
        const QVector<AppTraffic> snap = monitor.snapshot();
        QStringList rows;
        for (int i = 0; i < snap.size() && i < 6; ++i) {
            const AppTraffic &a = snap.at(i);
            rows << QStringLiteral("%1  down %2  up %3")
                        .arg(a.displayName.leftJustified(28),
                             theme::formatBytesPerSec(a.downBytesPerSec).rightJustified(12),
                             theme::formatBytesPerSec(a.upBytesPerSec).rightJustified(12));
        }
        r.item(Outcome::Pass,
               QStringLiteral("Applications attributed during the test"),
               rows.join(QLatin1Char('\n')));
        r.item(Outcome::Info, QStringLiteral("How to read this"),
               QStringLiteral("The busiest application above should be whichever one "
                              "was actually transferring. 'down' must be the larger "
                              "number while downloading, 'up' while uploading. If they "
                              "are swapped, the direction mapping has regressed - see "
                              "docs/RESOLVED-direction.md."));
    } else {
        // Nothing was attributed. That is only a FAILURE if the machine was busy:
        // an idle network legitimately produces an empty list, and reporting it as
        // a fault would be the same misdiagnosis in a new place.
        r.item(Outcome::Skip, QStringLiteral("Application attribution"),
               QStringLiteral("No application transferred data during the %1 second "
                              "window, so there was nothing to attribute. Run the test "
                              "again while a download or video is playing.")
                   .arg(seconds));
    }

    monitor.stop();
    r.item(monitor.state() == MonitorState::Stopped ? Outcome::Pass : Outcome::Fail,
           QStringLiteral("Monitoring released the session cleanly"),
           QStringLiteral("state after stop: %1")
               .arg(monitor.state() == MonitorState::Stopped ? QStringLiteral("Stopped")
                                                             : QStringLiteral("NOT stopped")));
}

// Confirms the driver is present and the packet path can be opened. Enforcement
// itself is not exercised here (it needs a real transfer to measure); this only
// proves the mechanism is available.
void checkDriver(Report &r)
{
    r.section(QStringLiteral("PACKET DRIVER (per-application limiting)"));

    const QString dir = QCoreApplication::applicationDirPath();
    const QString dll = dir + QStringLiteral("/WinDivert.dll");
    const QString sys = dir + QStringLiteral("/WinDivert64.sys");

    r.item(QFileInfo::exists(dll) ? Outcome::Pass : Outcome::Fail,
           QStringLiteral("WinDivert.dll present"), dll);
    r.item(QFileInfo::exists(sys) ? Outcome::Pass : Outcome::Fail,
           QStringLiteral("WinDivert64.sys present"), sys);

    // Ask the driver to open a handle. A failure here is exactly what the App
    // Limiter page reports as "the traffic driver could not be opened".
    ThrottleEngine engine;
    QString err;
    // apply() is the engine's entry point; it reports its own failure reason.
    QString engineError;
    QObject::connect(&engine, &ThrottleEngine::errorOccurred, &engine,
                     [&engineError](const QString &m) { engineError = m; });
    engine.apply(1024 * 1024, 1024 * 1024);
    const bool running = engine.isThrottling();
    r.item(running ? Outcome::Pass : Outcome::Fail,
           QStringLiteral("Driver opened for packet shaping"),
           running ? QStringLiteral("A WinDivert handle is open.")
                   : (engineError.isEmpty()
                          ? QStringLiteral("The driver refused a handle. The usual causes "
                                           "are a missing signature, a disabled driver "
                                           "service, or another product blocking it.")
                          : QStringLiteral("The driver refused a handle. It said: %1")
                                .arg(engineError)));
    if (running)
        engine.detach();
}

// Report the settings that affect enforcement, so the reader can see whether a
// schedule *should* be active right now.
void checkSchedules(Report &r)
{
    r.section(QStringLiteral("SCHEDULES"));

    SettingsStore store;
    const Settings s = store.load();

    r.item(Outcome::Info, QStringLiteral("System-wide limits"),
           s.enabled ? QStringLiteral("switched ON")
                     : QStringLiteral("switched OFF - no system-wide limiting is "
                                      "applied regardless of schedules"));

    const EffectiveLimits eff = net::evaluate(s, QDateTime::currentDateTime());
    r.item(Outcome::Info, QStringLiteral("Active now"),
           QStringLiteral("%1  (down %2/s, up %3/s)")
               .arg(eff.reason,
                    theme::formatBytesPerSec(eff.down),
                    theme::formatBytesPerSec(eff.up)));

    if (s.appRules.isEmpty()) {
        r.item(Outcome::Info, QStringLiteral("Per-application rules"),
               QStringLiteral("none configured"));
    } else {
        QStringList rows;
        for (const AppRule &a : s.appRules) {
            QString when = QStringLiteral("always on");
            if (!a.windows.isEmpty()) {
                QStringList wins;
                for (const Rule &w : a.windows) {
                    wins << QStringLiteral("%1-%2%3")
                                .arg(w.from, w.to,
                                     w.enabled ? QString() : QStringLiteral(" (disabled)"));
                }
                when = wins.join(QStringLiteral(", "));
            }
            rows << QStringLiteral("%1  %2  %3  down %4/s")
                        .arg(a.name.leftJustified(24),
                             a.enabled ? QStringLiteral("enabled ") : QStringLiteral("DISABLED"),
                             when.leftJustified(28),
                             theme::formatBytesPerSec(a.down));
        }
        r.item(Outcome::Info, QStringLiteral("Per-application rules"), rows.join(QLatin1Char('\n')));
        r.item(Outcome::Info, QStringLiteral("Note"),
               QStringLiteral("A rule whose schedule window is malformed never matches. "
                              "Times are stored as HH:MM; any other text is rejected by "
                              "the parser, which silently disables that window."));
    }
}

} // namespace

int runSelfTest(const QString &reportPath)
{
    Report r;
    r.lines << QStringLiteral("Flowbit self-test");
    r.lines << QStringLiteral("=================");
    r.lines << QStringLiteral("Run at: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    r.lines << QStringLiteral("Executable: %1").arg(QCoreApplication::applicationFilePath());

    r.section(QStringLiteral("BASICS"));
    r.item(Outcome::Info, QStringLiteral("Application"),
           QStringLiteral("Flowbit %1, Qt %2").arg(QCoreApplication::applicationVersion(),
                                                  QLatin1String(qVersion())));
    r.item(Outcome::Info, QStringLiteral("Windows"),
           QStringLiteral("build %1").arg(QSysInfo::kernelVersion()));

    const bool elevated = isElevated();
    r.item(elevated ? Outcome::Pass : Outcome::Fail,
           QStringLiteral("Elevated (Administrator)"),
           elevated ? QString()
                    : QStringLiteral("Both ETW monitoring and the driver need "
                                     "Administrator rights. Re-run this test from an "
                                     "elevated prompt, or right-click Flowbit.exe and "
                                     "choose 'Run as administrator'."));

    if (!elevated) {
        // Everything below would report a misleading failure, so stop here and say
        // so rather than producing a wall of red.
        r.section(QStringLiteral("RESULT"));
        r.lines << QStringLiteral("Could not run: Administrator rights are required.");
        const QString text = r.render();
        if (!reportPath.isEmpty()) {
            QFile f(reportPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream ts(&f);
                ts << text << Qt::endl;
            }
        }
        QTextStream out(stdout);
        out << text << Qt::endl;
        return 2;
    }

    // Default: a window long enough to catch real traffic but short enough to be
    // usable interactively.
    const int seconds = 20;

    checkTraceSession(r);
    checkDriver(r);
    checkMonitoring(r, seconds);
    checkSchedules(r);

    r.section(QStringLiteral("RESULT"));
    r.lines << QStringLiteral("passed: %1    failed: %2    skipped: %3")
                   .arg(r.passed).arg(r.failed).arg(r.skipped);
    if (r.failed == 0 && r.skipped == 0)
        r.lines << QStringLiteral("EVERY CHECK PASSED.");
    else if (r.failed == 0)
        r.lines << QStringLiteral("No failures, but some checks could not run - see [SK] "
                                  "lines above.");
    else
        r.lines << QStringLiteral("SOMETHING FAILED - see the [!!] lines above.");

    const QString text = r.render();
    if (!reportPath.isEmpty()) {
        QFile f(reportPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << text << Qt::endl;
        }
    }
    QTextStream out(stdout);
    out << text << Qt::endl;

    return r.failed == 0 ? 0 : 1;
}

} // namespace net
