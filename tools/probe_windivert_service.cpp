// Answers the question directly: does WinDivert need its service registered in advance,
// and does the service persist afterwards?
//
// The diagnostics page reports "WinDivert service: not registered" as a problem. If the
// driver creates that service on demand, then an absent service is a normal state and
// reporting it as a fault is wrong - and it would also mean users do NOT have to start
// Flowbit repeatedly to keep it working.
//
// This checks the service state, opens a WinDivert handle, checks again, closes it, and
// checks a third time. The three readings together say whether registration is on
// demand and whether it survives the handle being closed.

#include <windows.h>
#include <winsvc.h>

#include <cstdio>
#include <cstring>

// WinDivert's own header is C; declare just the two entry points this needs so the probe
// does not depend on the include path.
extern "C" {
__declspec(dllimport) HANDLE WinDivertOpen(const char *filter, int layer, short priority,
                                           unsigned long long flags);
__declspec(dllimport) BOOL WinDivertClose(HANDLE handle);
__declspec(dllimport) const char *WinDivertGetParam(HANDLE handle, int param);
}

namespace {

const char *serviceState()
{
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return "could not open the service manager";
    SC_HANDLE svc = OpenServiceA(scm, "WinDivert", SERVICE_QUERY_STATUS);
    if (!svc) {
        const DWORD err = GetLastError();
        CloseServiceHandle(scm);
        static char buf[96];
        if (err == ERROR_SERVICE_DOES_NOT_EXIST)
            return "NOT REGISTERED";
        std::snprintf(buf, sizeof(buf), "query failed (error %lu)", err);
        return buf;
    }
    SERVICE_STATUS status{};
    QueryServiceStatus(svc, &status);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    static char buf[96];
    const char *state = "unknown";
    switch (status.dwCurrentState) {
    case SERVICE_RUNNING: state = "RUNNING"; break;
    case SERVICE_STOPPED: state = "stopped"; break;
    case SERVICE_START_PENDING: state = "start pending"; break;
    case SERVICE_STOP_PENDING: state = "stop pending"; break;
    }
    std::snprintf(buf, sizeof(buf), "REGISTERED (%s)", state);
    return buf;
}

} // namespace

int main()
{
    std::printf("=== does WinDivert register its service on demand? ===\n\n");

    std::printf("  before opening a handle : %s\n", serviceState());

    // A filter that matches nothing, so the probe does not disturb real traffic; the
    // point is only to make the driver load.
    HANDLE h = WinDivertOpen("false", 0 /*NETWORK*/, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("  WinDivertOpen failed: error %lu\n", GetLastError());
        std::printf("  (a failed open means the driver could not start)\n");
        return 1;
    }
    std::printf("  WinDivertOpen           : SUCCEEDED\n");
    std::printf("  while the handle is open: %s\n", serviceState());

    WinDivertClose(h);
    std::printf("  after closing the handle: %s\n", serviceState());

    std::printf("\n  conclusion:\n");
    const char *s = serviceState();
    if (std::strstr(s, "NOT REGISTERED")) {
        std::printf("    The driver created the service to open a handle and removed it\n"
                    "    again afterwards. An absent service is therefore a NORMAL state,\n"
                    "    not a fault, and nothing needs starting in advance.\n");
    } else {
        std::printf("    The service remains registered after the handle is closed, so it\n"
                    "    is created once and stays.\n");
    }
    return 0;
}
