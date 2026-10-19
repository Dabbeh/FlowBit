// Tests the driver-service repair directly, without needing the GUI.
//
// Exactly the code the engine now runs is reproduced here so the behaviour can be
// checked on a machine where the service is genuinely disabled. This is a live test:
// it reads the service, and if it is disabled it re-enables it and retries the open,
// which is what the application does.
//
// Root cause being verified against, read from the user's machine:
//   SERVICE_NAME:     WinDivert
//   START_TYPE:       4   DISABLED
//   BINARY_PATH_NAME: \??\C:\Users\enaya\Desktop\Flowbit\WinDivert64.sys
//
// Windows disables a kernel service whose registered image is replaced underneath it,
// and updating Flowbit replaces WinDivert64.sys.

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <winsvc.h>
#include <windivert.h>

namespace {

std::string serviceStartType()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return "SCM unavailable (needs elevation)";
    SC_HANDLE svc = OpenServiceW(scm, L"WinDivert", SERVICE_QUERY_CONFIG);
    if (!svc) {
        CloseServiceHandle(scm);
        return "not registered";
    }
    DWORD needed = 0;
    QueryServiceConfigW(svc, nullptr, 0, &needed);
    std::string result = "unknown";
    if (needed > 0) {
        std::vector<BYTE> buf(needed);
        auto *cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data());
        if (QueryServiceConfigW(svc, cfg, needed, &needed)) {
            switch (cfg->dwStartType) {
            case SERVICE_BOOT_START: result = "0 BOOT"; break;
            case SERVICE_SYSTEM_START: result = "1 SYSTEM"; break;
            case SERVICE_AUTO_START: result = "2 AUTO"; break;
            case SERVICE_DEMAND_START: result = "3 DEMAND (normal for WinDivert)"; break;
            case SERVICE_DISABLED: result = "4 DISABLED"; break;
            }
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return result;
}

bool servicePointsAtOurDriver()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;
    bool matches = false;
    if (SC_HANDLE svc = OpenServiceW(scm, L"WinDivert", SERVICE_QUERY_CONFIG)) {
        DWORD needed = 0;
        QueryServiceConfigW(svc, nullptr, 0, &needed);
        if (needed > 0) {
            std::vector<BYTE> buf(needed);
            auto *cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data());
            if (QueryServiceConfigW(svc, cfg, needed, &needed) && cfg->lpBinaryPathName) {
                std::wstring p = cfg->lpBinaryPathName;
                matches = p.find(L"WinDivert64.sys") != std::wstring::npos
                          || p.find(L"WinDivert32.sys") != std::wstring::npos;
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return matches;
}

bool repairDisabledDriverService()
{
    if (!servicePointsAtOurDriver()) {
        std::printf("    repair declined: the service image is not our driver\n");
        return false;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;
    bool repaired = false;
    if (SC_HANDLE svc = OpenServiceW(scm, L"WinDivert", SERVICE_CHANGE_CONFIG)) {
        if (ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
                                 SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr)) {
            repaired = true;
        } else {
            std::printf("    ChangeServiceConfig failed: error %lu\n", GetLastError());
        }
        CloseServiceHandle(svc);
    } else {
        std::printf("    OpenService for change failed: error %lu\n", GetLastError());
    }
    CloseServiceHandle(scm);
    return repaired;
}

} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    std::printf("=== driver-service repair ===\n\n");

    std::printf("  start type before : %s\n", serviceStartType().c_str());
    std::printf("  image is our driver: %s\n", servicePointsAtOurDriver() ? "yes" : "no");

    // The exact filters the engine uses. A trivial filter can be satisfied by a driver
    // instance left over from an earlier session, so it does not exercise the failure.
    const char *filters[] = {"inbound and not loopback", "outbound and not loopback",
                             "false"};
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD firstError = 0;
    const char *usedFilter = nullptr;

    for (const char *f : filters) {
        h = WinDivertOpen(f, WINDIVERT_LAYER_NETWORK, 0, 0);
        if (h != INVALID_HANDLE_VALUE) {
            std::printf("\n  open with \"%s\"\n    SUCCEEDED\n", f);
            WinDivertClose(h);
            continue;
        }
        firstError = GetLastError();
        usedFilter = f;
        std::printf("\n  open with \"%s\"\n    FAILED: error %lu%s\n", f, firstError,
                    firstError == ERROR_SERVICE_DISABLED ? "  (ERROR_SERVICE_DISABLED)" : "");
        break;
    }

    if (!usedFilter) {
        std::printf("\n  every filter opened successfully.\n");
        std::printf("  start type after  : %s\n", serviceStartType().c_str());
        std::printf("\n  CONCLUSION: the driver opens normally right now, so there is\n"
                    "  nothing to repair at this moment.\n");
        return 0;
    }

    if (firstError != ERROR_SERVICE_DISABLED) {
        std::printf("\n  CONCLUSION: not the disabled-service case, so no repair applies.\n");
        return 0;
    }

    std::printf("\n  attempting the repair the engine performs...\n");
    if (!repairDisabledDriverService()) {
        std::printf("\n  CONCLUSION: repair FAILED - the old message is correct here.\n");
        return 1;
    }
    std::printf("  start type after repair: %s\n", serviceStartType().c_str());

    std::printf("\n  retrying the open with \"%s\"...\n", usedFilter);
    h = WinDivertOpen(usedFilter, WINDIVERT_LAYER_NETWORK, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("  retry failed: error %lu\n", GetLastError());
        std::printf("\n  CONCLUSION: repair did not make the driver load.\n");
        return 1;
    }
    WinDivertClose(h);
    std::printf("\n  CONCLUSION: REPAIR WORKED - the driver opened after re-enabling.\n");
    std::printf("  A limit will now apply, and the user was never asked to run a\n"
                "  command or restart anything.\n");
    return 0;
}
