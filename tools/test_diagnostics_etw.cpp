// The "ETW events received" row must distinguish four states that all report zero events.
//
// It used to show a WARNING - "the kernel network provider is delivering no events on this Windows
// build" - on every launch where the activity panel had not been opened, because the monitor is
// what starts the trace session and with no session there are no events by definition. A healthy
// machine was being told it had a fault.

#include "diagnostics.h"
#include "appmonitor.h"

#include <QCoreApplication>
#include <cstdio>

using namespace net;

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok) ++g_failures;
}

// Finds the row by name and returns it.
DiagnosticItem rowFor(const DiagnosticReport &r, const QString &name)
{
    for (const DiagnosticItem &i : r.items)
        if (i.name == name) return i;
    return {};
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("\n== zero ETW events: four states, one of them a fault ==\n");

    // 1. monitor never started - the exact case from the screenshot
    {
        const DiagnosticReport r = collectDiagnostics(true, QString(), 0, 0, false, 0);
        const DiagnosticItem i = rowFor(r, QStringLiteral("ETW events received"));
        std::printf("  not running      : %s | %s\n", qPrintable(i.value),
                    qPrintable(i.detail.left(70)));
        check(i.status == DiagnosticItem::Status::Info,
              "with the monitor stopped, zero events is information, not a warning");
        check(i.detail.contains(QStringLiteral("not running")),
              "and the detail says the monitor is what needs starting");
    }

    // 2. running, but only for a few seconds on an idle machine
    {
        const DiagnosticReport r = collectDiagnostics(true, QString(), 0, 0, true, 5);
        const DiagnosticItem i = rowFor(r, QStringLiteral("ETW events received"));
        std::printf("  running 5s       : %s\n", qPrintable(i.detail.left(70)));
        check(i.status == DiagnosticItem::Status::Info, "a few quiet seconds is not a fault");
        check(i.detail.contains(QStringLiteral("nothing has transferred")),
              "and says so plainly");
    }

    // 3. running a long time with no events at all - genuinely suspicious
    {
        const DiagnosticReport r = collectDiagnostics(true, QString(), 0, 0, true, 90);
        const DiagnosticItem i = rowFor(r, QStringLiteral("ETW events received"));
        std::printf("  running 90s      : %s\n", qPrintable(i.detail.left(70)));
        check(i.status == DiagnosticItem::Status::Warning,
              "a long quiet spell with no events is still worth a warning");
        check(i.detail.contains(QStringLiteral("90s")), "and the duration is named");
    }

    // 4. events arriving but not decoding - a real decoder fault
    {
        const DiagnosticReport r = collectDiagnostics(true, QString(), 40, 0, true, 30);
        const DiagnosticItem i = rowFor(r, QStringLiteral("ETW events received"));
        check(i.status == DiagnosticItem::Status::Ok,
              "events arriving is fine even if decoding is reported separately");
        const DiagnosticItem d = rowFor(r, QStringLiteral("Payload decode"));
        check(d.status == DiagnosticItem::Status::Problem,
              "events that do not decode remain a problem");
    }

    // 5. the counter is reported with its context, not as a bare "0 (decoded: 0)"
    {
        const DiagnosticReport r = collectDiagnostics(true, QString(), 12, 12, true, 45);
        const DiagnosticItem i = rowFor(r, QStringLiteral("ETW events received"));
        std::printf("  healthy          : %s\n", qPrintable(i.value));
        check(i.value.contains(QStringLiteral("45s")), "the value states how long it has run");
        check(i.status == DiagnosticItem::Status::Ok, "and a working source is OK");
    }

    std::printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
                g_failures == 0 ? "test_diagnostics_etw passed." : "FAILED.");
    return g_failures == 0 ? 0 : 1;
}
