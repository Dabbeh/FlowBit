// Probe: can WinDivert attribute network flows to a process, so packets can be
// throttled per application?
//
// Why this matters: WinDivert's NETWORK layer - where Flowbit's existing
// system-wide throttle runs - does not expose ProcessId. The documentation is
// explicit that Flow.ProcessId and Socket.ProcessId are only valid at their own
// layers. So a per-process filter cannot be expressed as a packet filter.
//
// The workable approach this probe validates:
//
//   1. Open a WINDIVERT_LAYER_FLOW handle. Each flow event carries ProcessId and
//      the local/remote 5-tuple for that flow.
//   2. Maintain a set of 5-tuples belonging to the processes we care about.
//   3. At the NETWORK layer, match packets against that set and throttle them.
//
// Step 3 is what the real implementation does; this probe establishes step 1 and
// 2, which is the part that determines whether the feature is possible at all.
//
// MUST RUN ELEVATED (- WinDivert needs Administrator).
// Include order matters, and it is subtle.
//
// windivert.h defines __in / __out / __inout as EMPTY macros for non-MSVC
// compilers. libstdc++ uses identifiers of the same shape internally (for
// example std::move(__in) inside <bits/stl_pair.h>), so pulling windivert.h in
// before the C++ standard headers rewrites them into calls with no arguments and
// the standard library fails to compile.
//
// The standard headers therefore come first, windivert.h last. This ordering is
// also why the existing throttleengine.cpp compiles: it happens not to use the
// affected templates.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "windivert.h"

namespace {

const char *protoName(UINT8 p)
{
    switch (p) {
    case 6:
        return "TCP";
    case 17:
        return "UDP";
    case 1:
        return "ICMP";
    default:
        return "?";
    }
}

std::string ipv4Of(const UINT32 addr[4])
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", addr[0] & 0xFF, (addr[0] >> 8) & 0xFF,
                  (addr[0] >> 16) & 0xFF, (addr[0] >> 24) & 0xFF);
    return buf;
}

std::string processName(DWORD pid)
{
    if (pid == 0)
        return "<system>";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return "<no access>";
    char buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    std::string result = "<unknown>";
    if (QueryFullProcessImageNameA(h, 0, buf, &size)) {
        const char *slash = std::strrchr(buf, '\\');
        result = slash ? slash + 1 : buf;
    }
    CloseHandle(h);
    return result;
}

} // namespace

int main(int argc, char *argv[])
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 8;

    std::printf("=== WinDivert per-process flow attribution probe ===\n\n");

    // ---------------------------------------------------------------------
    // 1. FLOW layer: does it really carry ProcessId?
    // ---------------------------------------------------------------------
    HANDLE flow = WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 0, 0);
    if (flow == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        std::printf("WinDivertOpen(FLOW) failed, error %lu\n", err);
        if (err == ERROR_ACCESS_DENIED)
            std::printf("  -> ACCESS_DENIED: this probe must run elevated.\n");
        else if (err == ERROR_FILE_NOT_FOUND)
            std::printf("  -> The WinDivert driver is not installed next to this exe.\n");
        return 1;
    }
    std::printf("FLOW layer handle opened.\n");

    // ---------------------------------------------------------------------
    // 2. NETWORK layer, the one packets are actually delayed at.
    // ---------------------------------------------------------------------
    HANDLE net = WinDivertOpen("true", WINDIVERT_LAYER_NETWORK, 0, 0);
    if (net == INVALID_HANDLE_VALUE) {
        std::printf("WinDivertOpen(NETWORK) failed, error %lu\n", GetLastError());
        WinDivertClose(flow);
        return 1;
    }
    std::printf("NETWORK layer handle opened.\n\n");

    std::printf("collecting flow events for %d seconds...\n\n", seconds);

    // pid -> number of flows seen, and the tuples attributed to it
    std::map<DWORD, int> flowsByPid;
    std::map<DWORD, std::set<std::string>> tuplesByPid;
    int flowEvents = 0;
    int nonFlowLayerData = 0;

    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(seconds) * 1000;

    std::vector<char> buffer(65535);
    while (GetTickCount64() < deadline) {
        UINT recvLen = 0;
        WINDIVERT_ADDRESS addr{};

        if (!WinDivertRecv(flow, buffer.data(), UINT(buffer.size()), &recvLen, &addr)) {
            const DWORD err = GetLastError();
            if (err == ERROR_NO_DATA)
                continue;
            std::printf("WinDivertRecv(FLOW) failed, error %lu\n", err);
            break;
        }

        // The layer field in the address says which layer produced this event.
        if (addr.Layer != WINDIVERT_LAYER_FLOW) {
            ++nonFlowLayerData;
            WinDivertSend(flow, buffer.data(), recvLen, nullptr, &addr);
            continue;
        }

        ++flowEvents;

        // Flow data lives in the address, not the packet payload.
        const WINDIVERT_DATA_FLOW &f = addr.Flow;
        const DWORD pid = f.ProcessId;

        ++flowsByPid[pid];

        char key[160];
        std::snprintf(key, sizeof(key), "%s:%u -> %s:%u %s", ipv4Of(f.LocalAddr).c_str(),
                      f.LocalPort, ipv4Of(f.RemoteAddr).c_str(), f.RemotePort,
                      protoName(UINT8(f.Protocol)));
        tuplesByPid[pid].insert(key);

        // Flow events must be re-injected so the stack is not stalled.
        WinDivertSend(flow, buffer.data(), recvLen, nullptr, &addr);
    }

    std::printf("=== result ===\n");
    std::printf("flow-layer events received : %d\n", flowEvents);
    std::printf("non-flow events on the flow handle : %d\n", nonFlowLayerData);
    std::printf("distinct processes with flows : %zu\n\n", flowsByPid.size());

    if (flowEvents == 0) {
        std::printf("NO FLOW EVENTS. Per-process attribution is not working here.\n");
        WinDivertClose(net);
        WinDivertClose(flow);
        return 1;
    }

    // Show the busiest few, with the tuples that a packet filter would need.
    std::vector<std::pair<int, DWORD>> ranked;
    for (const auto &kv : flowsByPid)
        ranked.push_back({kv.second, kv.first});
    std::sort(ranked.begin(), ranked.end(), std::greater<>());

    std::printf("%-8s %-26s %s\n", "PID", "PROCESS", "FLOWS");
    int shown = 0;
    for (const auto &r : ranked) {
        std::printf("%-8lu %-26s %d\n", r.second, processName(r.second).c_str(), r.first);
        if (++shown >= 10)
            break;
    }

    // Prove the tuples are usable as a filter: print a couple.
    std::printf("\nsample attributable 5-tuples:\n");
    shown = 0;
    for (const auto &kv : tuplesByPid) {
        for (const std::string &t : kv.second) {
            std::printf("  pid %-6lu %s\n", kv.first, t.c_str());
            if (++shown >= 6)
                break;
        }
        if (shown >= 6)
            break;
    }

    std::printf("\nCONCLUSION: the FLOW layer carried ProcessId for %zu processes,\n",
                flowsByPid.size());
    std::printf("so packets can be attributed to an application by matching their\n");
    std::printf("5-tuple against this set at the NETWORK layer.\n");

    WinDivertClose(net);
    WinDivertClose(flow);
    return 0;
}
