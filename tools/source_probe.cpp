// Tests every data source Flowbit could use to count per-application traffic, so
// we can pick one that is not blocked on this machine.
//
// Context: ETW trace-session creation is refused system-wide on this build (even
// Microsoft's own logman gets "Access is denied"), so the ETW source Flowbit uses
// cannot start. The question this answers: what CAN still be read?
//
//   1. SetPerTcpConnectionEStats / GetPerTcpConnectionEStats  - per-connection byte
//      counters, the fallback Flowbit already implements. Different API family from
//      ETW, so tamper protection generally leaves it alone.
//   2. GetExtendedTcpTable - connection table with owning PIDs (used for
//      attribution; also proves we can see processes at all).
//   3. GetIpStatisticsEx - machine-wide byte counters (a last-resort source).
//
// Run elevated. Exit code = number of working sources.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <netioapi.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

void testEstats()
{
    std::printf("\n--- 1. TCP EStats (per-connection byte counters) ---\n");

    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
        != ERROR_INSUFFICIENT_BUFFER) {
        std::printf("  could not size the TCP table\n");
        return;
    }

    std::vector<BYTE> buf(size);
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
        != NO_ERROR) {
        std::printf("  could not read the TCP table\n");
        return;
    }

    const DWORD count = *reinterpret_cast<DWORD *>(buf.data());
    auto *rows = reinterpret_cast<MIB_TCPROW_OWNER_PID *>(buf.data() + 4);
    std::printf("  connections visible: %lu\n", count);

    int tried = 0;
    int worked = 0;
    for (DWORD i = 0; i < count && tried < 5; ++i) {
        MIB_TCPROW row{};
        row.dwState = rows[i].dwState;
        row.dwLocalAddr = rows[i].dwLocalAddr;
        row.dwLocalPort = rows[i].dwLocalPort;
        row.dwRemoteAddr = rows[i].dwRemoteAddr;
        row.dwRemotePort = rows[i].dwRemotePort;

        TCP_ESTATS_DATA_RW_v0 rw{};
        rw.EnableCollection = TRUE;
        const DWORD erc = SetPerTcpConnectionEStats(
            &row, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0,
            sizeof(rw), 0);
        if (erc != NO_ERROR) {
            std::printf("  SetPerTcpConnectionEStats: error %lu\n", erc);
            ++tried;
            continue;
        }

        TCP_ESTATS_DATA_ROD_v0 rod{};
        const DWORD grc = GetPerTcpConnectionEStats(
            &row, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0,
            reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod));
        if (grc == NO_ERROR) {
            std::printf("  connection %d: DataBytesOut=%llu DataBytesIn=%llu  (READABLE)\n",
                        tried, static_cast<unsigned long long>(rod.DataBytesOut),
                        static_cast<unsigned long long>(rod.DataBytesIn));
            ++worked;
        } else {
            std::printf("  connection %d: GetPerTcpConnectionEStats error %lu\n", tried, grc);
        }
        ++tried;
    }

    std::printf("  EStats verdict: %s (%d/%d readable)\n",
                worked > 0 ? "WORKS" : "BLOCKED", worked, tried);
}

void testTable()
{
    std::printf("\n--- 2. Connection table with PIDs (attribution) ---\n");
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
        == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buf(size);
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
            == NO_ERROR) {
            const DWORD count = *reinterpret_cast<DWORD *>(buf.data());
            std::printf("  %lu TCP rows readable with owning PIDs -> WORKS\n", count);
            return;
        }
    }
    std::printf("  BLOCKED\n");
}

void testIpStats()
{
    std::printf("\n--- 3. Machine-wide IP statistics ---\n");
    MIB_IPSTATS stats{};
    if (GetIpStatisticsEx(&stats, AF_INET) == NO_ERROR) {
        MIB_IPSTATS *s = &stats;
        std::printf("  dwInReceives=%lu dwOutRequests=%lu -> WORKS\n",
                    s->dwInReceives, s->dwOutRequests);
    } else {
        std::printf("  BLOCKED (error %lu)\n", GetLastError());
    }
}

} // namespace

int main()
{
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::printf("=== data source probe ===\n");
    std::printf("elevated: %s\n", [] {
        HANDLE t = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t))
            return "unknown";
        TOKEN_ELEVATION e{};
        DWORD s = sizeof(e);
        const bool ok = GetTokenInformation(t, TokenElevation, &e, s, &s) && e.TokenIsElevated;
        CloseHandle(t);
        return ok ? "YES" : "no";
    }());

    testEstats();
    testTable();
    testIpStats();

    std::printf("\n=== done ===\n");
    WSACleanup();
    return 0;
}
