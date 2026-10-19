// Diagnoses what ControlTrace QUERY actually returns on this machine, which is
// what made the diagnostics report a "leftover trace session" that did not exist.
//
// Known from the field: leftoverSessionExists() reads true while logman shows zero
// sessions and even `logman create trace` is refused with Access Denied. So the
// QUERY return code must be something OTHER than 4201 (not found) or SUCCESS.
//
// This prints the raw code for: an absent name, the legacy Flowbit name, and a
// StartTrace attempt - the three inputs the old classification depended on.

#include <windows.h>
#include <evntrace.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

ULONG queryCode(const wchar_t *name)
{
    const ULONG bufSize = sizeof(EVENT_TRACE_PROPERTIES) + 2048 * sizeof(wchar_t);
    std::vector<BYTE> buf(bufSize, 0);
    auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
    props->Wnode.BufferSize = bufSize;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return ControlTraceW(0, name, props, EVENT_TRACE_CONTROL_QUERY);
}

const char *explain(ULONG rc)
{
    switch (rc) {
    case ERROR_SUCCESS:            return "SUCCESS (session exists)";
    case 4201:                     return "4201 ERROR_WMI_INSTANCE_NOT_FOUND (absent)";
    case ERROR_ACCESS_DENIED:      return "5 ACCESS_DENIED (API blocked, NOT proof of existence)";
    case ERROR_MORE_DATA:          return "234 ERROR_MORE_DATA";
    case ERROR_BUFFER_OVERFLOW:    return "111 ERROR_BUFFER_OVERFLOW";
    default:                       return "(other)";
    }
}

} // namespace

int main()
{
    HANDLE token = nullptr;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD s = sizeof(e);
        elevated = GetTokenInformation(token, TokenElevation, &e, s, &s) && e.TokenIsElevated;
        CloseHandle(token);
    }
    std::printf("elevated: %s\n\n", elevated ? "YES" : "no");

    struct Case { const wchar_t *name; const char *label; };
    const Case cases[] = {
        {L"Flowbit-Probe-Absent-Name-XYZ", "name that cannot exist"},
        {L"Flowbit-AppMonitor",            "legacy Flowbit name"},
        {L"NT Kernel Logger",              "the kernel logger itself"},
    };

    for (const Case &c : cases) {
        const ULONG rc = queryCode(c.name);
        std::printf("QUERY  %-28s (%s)\n", c.label, explain(rc));
    }

    std::printf("\ninterpretation:\n");
    const ULONG absent = queryCode(L"Flowbit-Probe-Absent-Name-XYZ");
    if (absent == ERROR_ACCESS_DENIED) {
        std::printf("  The ETW control API is BLOCKED on this machine: it answers\n"
                    "  ACCESS_DENIED for a name that cannot exist. Code 5 must never be\n"
                    "  read as \"a session exists\" - it means the query itself was refused,\n"
                    "  so the presence of a leftover session is UNKNOWN, not confirmed.\n");
    } else if (absent == 4201) {
        std::printf("  Query works normally (absent -> 4201). A \"leftover\" verdict here\n"
                    "  would be genuine.\n");
    }
    return 0;
}
