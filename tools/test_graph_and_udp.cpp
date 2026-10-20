// Event classification, and the graph's average series.
//
// The decoder used to recognise only TCP send/receive by hardcoded ids, which made UDP invisible
// (DNS, QUIC, WireGuard, games, most calls). It now classifies by the event's own name, with the id
// table kept as the fallback for records that carry no name. These checks pin both halves.

#include "etwclass.h"
#include "speedgraph.h"

#include <QApplication>
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
} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    std::printf("\n== ETW event classification ==\n");

    // ---- TCP, by name --------------------------------------------------
    {
        const auto k = etwclass::classify(L"KNetEvt_TcpSend", 10);
        check(k.transfer && k.send && !k.udp && !k.retransmit, "TcpSend is a send, not UDP");
        check(k.fromName, "and was classified by name");
    }
    {
        const auto k = etwclass::classify(L"KNetEvt_TcpRecv", 11);
        check(k.transfer && !k.send && !k.udp, "TcpRecv is a receive");
    }
    {
        const auto k = etwclass::classify(L"KNetEvt_TcpRetransmit", 14);
        check(k.transfer && k.retransmit, "TcpRetransmit is marked as a retransmission");
    }

    // ---- UDP, which the id table could never reach ---------------------
    {
        const auto k = etwclass::classify(L"KNetEvt_UdpSend", 42);
        check(k.transfer && k.send && k.udp, "UdpSend is a UDP send");
    }
    {
        const auto k = etwclass::classify(L"KNetEvt_UdpRecv", 43);
        check(k.transfer && !k.send && k.udp, "UdpRecv is a UDP receive");
    }
    {
        const auto k = etwclass::classify(L"KNetEvt_UdpSendIPV6", 44);
        check(k.transfer && k.send && k.udp, "the IPv6 UDP variant is recognised too");
    }

    // ---- control events carry no bytes --------------------------------
    {
        const auto a = etwclass::classify(L"KNetEvt_TcpConnect", 12);
        const auto b = etwclass::classify(L"KNetEvt_TcpDisconnect", 13);
        const auto c = etwclass::classify(L"KNetEvt_TcpFailure", 17);
        check(!a.transfer && !b.transfer && !c.transfer,
              "connect / disconnect / failure are not byte carriers");
    }

    // ---- fallback when a record has no name ---------------------------
    {
        const auto a = etwclass::classify(nullptr, 10);
        const auto b = etwclass::classify(nullptr, 27);
        const auto c = etwclass::classify(nullptr, 30);
        const auto d = etwclass::classify(nullptr, 99);
        check(a.transfer && a.send && !a.fromName, "id 10 still classifies as a send without a name");
        check(b.transfer && !b.send, "id 27 (IPv6 receive) still classifies as a receive");
        check(c.retransmit, "id 30 is still a retransmission");
        check(!d.transfer, "an unknown id with no name is ignored, as before");
    }

    std::printf("\n== graph averages ==\n");
    {
        SpeedGraph g;
        check(g.averageDown() == 0.0 && g.averageUp() == 0.0, "an empty graph averages zero");
        g.addSample(100, 10);
        g.addSample(300, 30);
        std::printf("  down avg %.1f, up avg %.1f\n", g.averageDown(), g.averageUp());
        check(qFuzzyCompare(g.averageDown() + 1.0, 201.0), "download average is the mean of the window");
        check(qFuzzyCompare(g.averageUp() + 1.0, 21.0), "upload average is the mean of the window");
        check(g.sizeHint().height() > 120, "the graph asks for a taller area than before");
        check(g.sizePolicy().verticalPolicy() == QSizePolicy::Expanding,
              "and is allowed to grow with the window");
        g.clearHistory();
        check(g.averageDown() == 0.0, "clearing the history clears the averages");
    }

    std::printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
                g_failures == 0 ? "test_graph_and_udp passed." : "FAILED.");
    return g_failures == 0 ? 0 : 1;
}
