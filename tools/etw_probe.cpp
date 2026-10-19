// Minimal ETW probe: calls StartTrace with a guaranteed-fresh session name and
// prints the raw return code, plus the privileges held by this process.
//
// Purpose: the application reports ACCESS_DENIED (5) from StartTrace AND from
// ControlTrace STOP - the latter on a session that does not exist, which should
// answer ERROR_WMI_INSTANCE_NOT_FOUND (4201). An API answering "denied" for a
// non-existent object points at a privilege or policy block, not at a leftover.
//
// Run elevated. Any nonzero StartTrace code here is environmental.

#include <windows.h>
#include <evntrace.h>
#include <evntprov.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool hasPrivilege(const wchar_t *name)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    DWORD size = 0;
    GetTokenInformation(token, TokenPrivileges, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    bool found = false;
    if (GetTokenInformation(token, TokenPrivileges, buf.data(), size, &size)) {
        auto *privs = reinterpret_cast<TOKEN_PRIVILEGES *>(buf.data());
        for (DWORD i = 0; i < privs->PrivilegeCount; ++i) {
            wchar_t text[256] = {};
            DWORD len = 256;
            if (LookupPrivilegeNameW(nullptr, &privs->Privileges[i].Luid, text, &len)
                && _wcsicmp(text, name) == 0) {
                found = true;
                break;
            }
        }
    }
    CloseHandle(token);
    return found;
}

bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, size, &size)
                    && elev.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

} // namespace

int main()
{
    std::printf("=== ETW probe ===\n");
    std::printf("elevated            : %s\n", isElevated() ? "YES" : "no");

    struct Priv { const wchar_t *name; };
    const wchar_t *privs[] = {L"SeSystemProfilePrivilege",
                              L"SeProfileSingleProcessPrivilege",
                              L"SeDebugPrivilege",
                              L"SeBackupPrivilege"};
    for (const wchar_t *pn : privs) {
        char narrow[128] = {};
        WideCharToMultiByte(CP_ACP, 0, pn, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        std::printf("  %-34s: %s\n", narrow, hasPrivilege(pn) ? "held" : "ABSENT");
    }

    // 1. ControlTrace STOP on a name that cannot exist.
    {
        const ULONG bufSize = sizeof(EVENT_TRACE_PROPERTIES) + 256 * sizeof(wchar_t);
        std::vector<BYTE> buf(bufSize, 0);
        auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
        props->Wnode.BufferSize = bufSize;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        const ULONG rc = ControlTraceW(0, L"Flowbit-Probe-DoesNotExist-0001", props,
                                       EVENT_TRACE_CONTROL_STOP);
        std::printf("\nControlTrace STOP on a non-existent session: %lu", rc);
        if (rc == 4201)
            std::printf("  (4201 = not found: correct)\n");
        else if (rc == 5)
            std::printf("  (5 = ACCESS DENIED even though nothing exists -> blocked)\n");
        else
            std::printf("  (unexpected)\n");
    }

    // 2. StartTrace with a virgin name. This is the operation the app needs.
    {
        const std::wstring name =
            L"Flowbit-Probe-" + std::to_wstring(GetCurrentProcessId());
        const ULONG bufSize = ULONG(sizeof(EVENT_TRACE_PROPERTIES)
                                    + (name.size() + 1) * sizeof(wchar_t));
        std::vector<BYTE> buf(bufSize, 0);
        auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
        props->Wnode.BufferSize = bufSize;
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        props->BufferSize = 64;
        props->MinimumBuffers = 4;
        props->MaximumBuffers = 16;
        props->FlushTimer = 1;

        auto *nameField = reinterpret_cast<wchar_t *>(buf.data() + props->LoggerNameOffset);
        memcpy(nameField, name.c_str(), (name.size() + 1) * sizeof(wchar_t));

        TRACEHANDLE session = 0;
        const ULONG rc = StartTraceW(&session, nameField, props);
        std::printf("\nStartTrace (virgin name, buffer sized from the name): %lu", rc);
        if (rc == ERROR_SUCCESS)
            std::printf("  (0 = SUCCESS)\n");
        else if (rc == 5)
            std::printf("  (5 = ACCESS DENIED)\n");
        else if (rc == 24)
            std::printf("  (24 = BAD_LENGTH - sizing bug)\n");
        else
            std::printf("  (other)\n");

        if (rc == ERROR_SUCCESS) {
            // 3. Can the kernel network provider be enabled on it?
            const GUID kernelNetwork = {
                0x7dd42a49, 0x5329, 0x4832,
                {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
            const ULONG erc = EnableTraceEx2(session, &kernelNetwork,
                                             EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                             TRACE_LEVEL_VERBOSE,
                                             0xFFFFFFFFFFFFFFFFULL, 0, 0, nullptr);
            std::printf("EnableTraceEx2(kernel-network, VERBOSE): %lu %s\n", erc,
                        erc == ERROR_SUCCESS ? "(SUCCESS)" : "(FAILED)");

            const ULONG src = ControlTraceW(session, nameField, props, EVENT_TRACE_CONTROL_STOP);
            std::printf("STOP: %lu %s\n", src, src == ERROR_SUCCESS ? "(SUCCESS)" : "(FAILED)");
        }
    }

    std::printf("\n=== done ===\n");
    return 0;
}
