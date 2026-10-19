// Verifies that a disabled WinDivert service is repaired at startup.
//
// The condition being reproduced is the one on this machine:
//
//     START_TYPE : 4  DISABLED      Windows will refuse to start it
//     STATE      : 4  RUNNING       but it is loaded in kernel memory
//
// A driver that is already loaded opens successfully whatever the start type says, so the
// fault is invisible until the NEXT load. This test therefore checks the start type in the
// registry rather than trying to open the driver: that is the value that decides whether a
// reboot would leave the machine with no limits at all.
//
// The test SETS the start type to 4 itself and restores the original value afterwards, so
// running it does not leave the machine in the broken state it is checking for.

#include "throttleengine.h"

#include <QCoreApplication>

#include <windows.h>

#include <cstdio>

using namespace net;

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

const wchar_t *kKey = L"SYSTEM\\CurrentControlSet\\Services\\WinDivert";

DWORD readStart()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0xFFFFFFFF;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG r = RegQueryValueExW(key, L"Start", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS ? value : 0xFFFFFFFF;
}

bool writeStart(DWORD value)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const LONG r = RegSetValueExW(key, L"Start", 0, REG_DWORD,
                                  reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

// The registry write above needs elevation; this reports that rather than failing obscurely.
bool elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION e{};
    DWORD size = sizeof(e);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &e, size, &size);
    CloseHandle(token);
    return ok && e.TokenIsElevated != 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    std::printf("\n== WinDivert service readiness ==\n");

    const DWORD original = readStart();
    if (original == 0xFFFFFFFF) {
        std::printf("  the WinDivert service is not registered, so there is nothing to test\n");
        std::printf("  (this is normal on a machine where Flowbit has never shaped traffic)\n");
        return 0;
    }
    std::printf("  start type as found : %lu%s\n", original,
                original == 4 ? "  (DISABLED - the broken state)" : "");

    if (!elevated()) {
        // The check itself is what matters, and it must still be correct when it cannot
        // change anything - a non-elevated Flowbit is a supported state, not a crash.
        ThrottleEngine engine;
        const auto outcome = engine.ensureDriverReady();
        check(outcome != ThrottleEngine::DriverReadiness::Ready || original != 4,
              "without elevation the check still answers rather than failing");
        std::printf("  not elevated, so the repair cannot be exercised here\n");
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    // ---- the repair ------------------------------------------------------
    if (!writeStart(4)) {
        std::printf("  could not set the start type to DISABLED\n");
        return 1;
    }
    std::printf("  set to 4 (DISABLED) to reproduce the reported state\n");
    check(readStart() == 4, "the service is now disabled, as on a broken machine");

    {
        ThrottleEngine engine;
        const auto outcome = engine.ensureDriverReady();

        const char *name = "?";
        switch (outcome) {
        case ThrottleEngine::DriverReadiness::Ready:        name = "Ready"; break;
        case ThrottleEngine::DriverReadiness::Repaired:     name = "Repaired"; break;
        case ThrottleEngine::DriverReadiness::Held:         name = "Held"; break;
        case ThrottleEngine::DriverReadiness::NeedsRestart: name = "NeedsRestart"; break;
        case ThrottleEngine::DriverReadiness::Unavailable:  name = "Unavailable"; break;
        }
        std::printf("  ensureDriverReady() -> %s\n", name);

        check(outcome == ThrottleEngine::DriverReadiness::Repaired,
              "it reports the service as repaired");
        check(readStart() != 4,
              "and the registry start type is no longer DISABLED - so a reboot would work");

        const DWORD now = readStart();
        std::printf("  start type now      : %lu%s\n", now,
                    now == 3 ? "  (DEMAND_START - correct)" : "");
    }

    // ---- a second call is harmless ---------------------------------------
    {
        ThrottleEngine engine;
        const auto outcome = engine.ensureDriverReady();
        check(outcome == ThrottleEngine::DriverReadiness::Ready,
              "a second check on a healthy service reports Ready and changes nothing");
    }

    // ---- the service is left as it was found -----------------------------
    writeStart(original);
    std::printf("  restored start type : %lu (as found)\n", readStart());
    check(readStart() == original, "the original start type is restored");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_driver_ready passed."
                                        : "test_driver_ready FAILED.");
    return g_failures == 0 ? 0 : 1;
}
