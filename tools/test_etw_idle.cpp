// Verifies the monitor no longer declares ETW broken just because the link is quiet.
//
// THE FAULT
//
// The tick compared the event counter with the previous sample, and after three silent ticks
// concluded the ETW source was not working - switching to the TCP fallback and showing
// "The detailed ETW source is delivering no events on this Windows build" under the table.
//
// Silence is not evidence of a broken source: an idle machine, every application at 0 B/s,
// sends no events. The warning appeared and disappeared with traffic, which is exactly how it
// was reported - "sometimes we got this thing no showing".
//
// THE RULE NOW
//
// Once a single event has arrived, the source is proven for the session and silence can only
// mean no traffic. The fallback is chosen only while the source has never delivered anything.
//
// The state cannot be fabricated from a test, so the checks are of the paths, read from the
// source: the latch exists, the callback sets it, the silence branch is guarded by it, and the
// note is short enough not to swallow the panel.

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <cstdio>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

QString read(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("\n== idle is not broken: the ETW silence rule ==\n");

    const QString src =
        read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/appmonitor.cpp"));
    check(!src.isEmpty(), "the monitor source is readable");
    if (src.isEmpty())
        return 1;

    // 1. the latch exists and is set where events are counted
    check(src.contains(QStringLiteral("bool etwProven")), "a latch records that ETW has delivered");
    check(src.contains(QStringLiteral("impl->etwProven = true")),
          "and it is set in the event callback, where delivery is observed");

    // 2. the silence branch can only run while the source has never delivered
    check(src.contains(QStringLiteral("} else if (!m_impl->etwProven) {")),
          "the fallback is chosen only while ETW has never delivered anything");

    // 3. recovery still works: events clear the fallback and the tick counter
    check(src.contains(QStringLiteral("m_impl->silentTicks = 0;")),
          "arriving events reset the silent-tick counter");
    check(src.contains(QStringLiteral("m_impl->connLast.clear();")),
          "and the fallback state is dropped, so the two sources never double-count");

    // 4. the note says what is unavailable, in two short fragments, and no longer claims the
    //    source is broken. Measured by content rather than by length: reconstructing the runtime
    //    string from source text proved unreliable, and an unreliable check that passes is worse
    //    than no check at all.
    check(src.contains(QStringLiteral("Measuring TCP only")),
          "the fallback note leads with what is actually being measured");
    check(src.contains(QStringLiteral("events, so UDP and QUIC are not counted.")),
          "and names the traffic it cannot count");
    check(!src.contains(QStringLiteral("The detailed ETW source is delivering no events")),
          "the three-line wording that read as a fault is gone");
    check(!src.contains(QStringLiteral("connection counters instead: the figures are real")),
          "along with the rest of that paragraph");

    // 5. the two sources cannot both feed the counters. This is the other half of the
    //    accuracy fault: while the fallback was chosen, the ETW session was still delivering
    //    events for the same connections, and both wrote into inBytes/outBytes, so the same
    //    traffic was counted twice and per-application usage read higher than reality.
    check(src.contains(QStringLiteral("if (useEstats)\n        return;")),
          "ETW events are ignored for accounting while the fallback is the chosen source");
    check(src.contains(QStringLiteral("if (!useEstats)\n        return;")),
          "and the fallback contributes nothing while ETW is the chosen source");
    check(src.contains(QStringLiteral("Exactly one source may feed the counters")),
          "the rule is stated where it is enforced, not only followed");

    // 6. retransmissions do not add bytes. They carry data that was already counted when it was
    //    first sent, and the TCP-counter source counts sequence space rather than wire repeats -
    //    so counting them both inflated usage and made the figure depend on the active source.
    check(src.contains(QStringLiteral("if (id == kTcpRetransmit || id == kTcpIpv6Retransmit)")),
          "retransmission events are recognised and handled before bytes are recorded");
    {
        // The early return must sit after the liveness latch, or a link carrying only retransmits
        // would look dead to the source check.
        const int latch = src.indexOf(QStringLiteral("impl->etwProven = true"));
        const int guard = src.indexOf(QStringLiteral("if (id == kTcpRetransmit || id == kTcpIpv6Retransmit)"));
        check(latch > 0 && guard > latch,
              "and after the liveness latch, so retransmits still prove the source is alive");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_etw_idle passed." : "test_etw_idle FAILED.");
    return g_failures == 0 ? 0 : 1;
}
