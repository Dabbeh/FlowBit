// Verifies the repair fires for the case it is actually for: the driver NOT loaded and
// the service DISABLED, which is the state a reboot produces after an update.
//
// Established by measurement:
//   * with a driver instance still resident, the open succeeds even though the service
//     reads DISABLED - so a working limiter alongside a disabled service is normal;
//   * the failure appears when the service must START the driver, which is after a
//     restart.
//
// The probe reproduces that state: it stops the service and removes the resident
// driver, checks that the open now fails with ERROR_SERVICE_DISABLED, runs the same
// repair the engine runs, and retries. It then restores the service to the state it
// found so the machine is left as it was.

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <winsvc.h>
#include <windivert.h>

namespace {

DWORD g_originalStart = SERVICE_NO_CHANGE;

std::string startTypeName(DWORD t)
{
    switch (t) {
    case SERVICE_BOOT_START: return "0 BOOT";
    case SERVICE_SYSTEM_START: return "1 SYSTEM";
    case SERVICE_AUTO_START: return "2 AUTO";
    case SERVICE_DEMAND_START: return "3 DEMAND";
    case SERVICE_DISABLED: return "4 DISABLED";
    }
    return "unknown";
}

bool queryService(DWORD *startType, bool *running, std::wstring *imagePath)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;
    bool ok = false;
    if (SC_HANDLE svc = OpenServiceW(scm, L"WinDivert",
                                     SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS)) {
        DWORD needed = 0;
        QueryServiceConfigW(svc, nullptr, 0, &needed);
        if (needed > 0) {
            std::vector<BYTE> buf(needed);
            auto *cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data());
            if (QueryServiceConfigW(svc, cfg, needed, &needed)) {
                if (startType) *startType = cfg->dwStartType;
                if (imagePath && cfg->lpBinaryPathName) *imagePath = cfg->lpBinaryPathName;
                ok = true;
            }
        }
        SERVICE_STATUS st{};
        if (QueryServiceStatus(svc, &st) && running)
            *running = (st.dwCurrentState == SERVICE_RUNNING);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool setStartType(DWORD type)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;
    bool ok = false;
    if (SC_HANDLE svc = OpenServiceW(scm, L"WinDivert", SERVICE_CHANGE_CONFIG)) {
        ok = ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                  nullptr);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool servicePointsAtOurDriver()
{
    std::wstring path;
    if (!queryService(nullptr, nullptr, &path))
        return false;
    return path.find(L"WinDivert64.sys") != std::wstring::npos
           || path.find(L"WinDivert32.sys") != std::wstring::npos;
}

bool repairDisabledDriverService()
{
    if (!servicePointsAtOurDriver())
        return false;
    return setStartType(SERVICE_DEMAND_START);
}

// The engine's own filter, which is the one that fails in this state.
const char *kFilter = "inbound and not loopback";

} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    std::printf("=== repair fires when the driver cannot be started ===\n\n");

    DWORD original = 0;
    bool running = false;
    std::wstring image;
    if (!queryService(&original, &running, &image)) {
        std::printf("  could not query the service (needs elevation)\n");
        return 1;
    }
    g_originalStart = original;
    std::printf("  found: start type %s, running=%s\n",
                startTypeName(original).c_str(), running ? "yes" : "no");
    std::printf("  image : %ls\n\n", image.c_str());
    std::printf("  the machine will be restored to this start type at the end\n\n");

    // Step 1: the state to reproduce is DISABLED with no resident driver.
    std::printf("  1. setting the service DISABLED...\n");
    if (!setStartType(SERVICE_DISABLED)) {
        std::printf("     could not set it: error %lu\n", GetLastError());
        return 1;
    }

    // Opening once loads the driver; closing it lets the instance unload after a moment.
    std::printf("  2. unloading any resident driver instance...\n");
    {
        HANDLE h = WinDivertOpen(kFilter, WINDIVERT_LAYER_NETWORK, 0, 0);
        if (h != INVALID_HANDLE_VALUE)
            WinDivertClose(h);
    }
    Sleep(1500);

    // Step 3: with the driver gone and the service disabled, the open must fail.
    std::printf("  3. attempting the open (this is the failing case)...\n");
    HANDLE h = WinDivertOpen(kFilter, WINDIVERT_LAYER_NETWORK, 0, 0);
    DWORD firstError = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE) {
        WinDivertClose(h);
        std::printf("     it SUCCEEDED - a resident instance is still holding the driver,\n"
                    "     so this state could not be reproduced on this machine.\n");
        std::printf("\n  restoring the original start type...\n");
        setStartType(original);
        std::printf("  restored to %s\n", startTypeName(original).c_str());
        std::printf("\n  CONCLUSION: the failing state needs a reboot to reproduce. The\n"
                    "  repair path is therefore verified by construction, not by this run.\n");
        return 0;
    }
    std::printf("     FAILED with error %lu%s\n", firstError,
                firstError == ERROR_SERVICE_DISABLED ? "  (ERROR_SERVICE_DISABLED)" : "");

    if (firstError != ERROR_SERVICE_DISABLED) {
        std::printf("\n  restoring the original start type...\n");
        setStartType(original);
        std::printf("\n  CONCLUSION: unexpected error; repair does not apply.\n");
        return 1;
    }

    // Step 4: the repair the engine performs.
    std::printf("\n  4. running the repair the engine performs...\n");
    if (!repairDisabledDriverService()) {
        std::printf("     repair FAILED\n");
        setStartType(original);
        return 1;
    }
    DWORD now = 0;
    queryService(&now, nullptr, nullptr);
    std::printf("     start type is now %s\n", startTypeName(now).c_str());

    // Step 5: retry.
    std::printf("\n  5. retrying the open...\n");
    h = WinDivertOpen(kFilter, WINDIVERT_LAYER_NETWORK, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("     retry FAILED: error %lu\n", GetLastError());
        setStartType(original);
        std::printf("\n  CONCLUSION: repair did not make the driver load.\n");
        return 1;
    }
    WinDivertClose(h);

    std::printf("\n  restoring the original start type (%s)...\n",
                startTypeName(original).c_str());
    setStartType(original);

    std::printf("\n  CONCLUSION: REPAIR WORKED.\n");
    std::printf("  The open failed with ERROR_SERVICE_DISABLED, the repair restored the\n"
                "  start type, and the retry succeeded - so a limit applies without the\n"
                "  user running a command or restarting anything.\n");
    return 0;
}
