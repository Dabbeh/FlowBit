// Determines the true reason StartTrace is refused on this machine.
//
// Established so far:
//   * A legacy "Flowbit-AppMonitor" session EXISTS (ControlTrace QUERY -> SUCCESS).
//   * logman cannot see or stop it ("Data Collector Set was not found").
//   * logman create trace is refused with Access Denied.
//   * StartTrace with a VIRGIN name returned 5 (ACCESS_DENIED) from an elevated
//     process holding every relevant privilege.
//
// The last point is the anomaly: a virgin name cannot collide, so a name clash
// cannot explain it. This probe isolates the cause by varying ONE thing at a time:
//
//   A. StartTrace with a virgin name, full REAL_TIME_MODE        (the app's call)
//   B. StartTrace with a virgin name, but a tiny MinimumBuffers  (resource-related?)
//   C. StartTrace with a virgin name, but the session's security descriptor omitted
//   D. StartTrace with the legacy name (expect collision), to see its code
//   E. StartTrace for an EVENT_TRACE_PRIVATE_LOGGER_MODE session (no global name)
//
// Whichever variant succeeds tells us the constraint. E is especially interesting:
// private loggers are per-process and may be exempt from whatever is blocking.

#include <windows.h>
#include <evntrace.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

const char *explain(ULONG rc)
{
    switch (rc) {
    case ERROR_SUCCESS:         return "SUCCESS";
    case ERROR_ACCESS_DENIED:   return "5 ACCESS_DENIED";
    case ERROR_ALREADY_EXISTS:  return "183 ALREADY_EXISTS";
    case ERROR_BAD_LENGTH:      return "24 BAD_LENGTH";
    case ERROR_INVALID_PARAMETER: return "87 INVALID_PARAMETER";
    case ERROR_NO_SYSTEM_RESOURCES: return "1450 NO_SYSTEM_RESOURCES";
    default:                    return "(other)";
    }
}

ULONG tryStart(const wchar_t *name, ULONG logFileMode, ULONG minBuffers,
               ULONG maxBuffers, TRACEHANDLE *outSession)
{
    const std::wstring n(name);
    const ULONG bufSize = ULONG(sizeof(EVENT_TRACE_PROPERTIES)
                                + (n.size() + 1) * sizeof(wchar_t));
    std::vector<BYTE> buf(bufSize, 0);
    auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
    props->Wnode.BufferSize = bufSize;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;
    props->LogFileMode = logFileMode;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize = 64;
    props->MinimumBuffers = minBuffers;
    props->MaximumBuffers = maxBuffers;
    props->FlushTimer = 1;

    auto *nameField = reinterpret_cast<wchar_t *>(buf.data() + props->LoggerNameOffset);
    memcpy(nameField, n.c_str(), (n.size() + 1) * sizeof(wchar_t));

    TRACEHANDLE s = 0;
    const ULONG rc = StartTraceW(&s, nameField, props);
    if (rc == ERROR_SUCCESS && outSession)
        *outSession = s;
    return rc;
}

} // namespace

int main()
{
    const DWORD pid = GetCurrentProcessId();
    wchar_t nameA[128], nameB[128], nameC[128], nameE[128];
    swprintf(nameA, 128, L"Flowbit-A-%lu", pid);
    swprintf(nameB, 128, L"Flowbit-B-%lu", pid);
    swprintf(nameC, 128, L"Flowbit-C-%lu", pid);
    swprintf(nameE, 128, L"Flowbit-E-%lu", pid);

    TRACEHANDLE session = 0;

    std::printf("=== StartTrace variants ===\n\n");

    ULONG rc = tryStart(nameA, EVENT_TRACE_REAL_TIME_MODE, 4, 16, &session);
    std::printf("A. virgin name, REAL_TIME_MODE, 4/16 buffers : %lu %s\n", rc, explain(rc));
    if (rc == ERROR_SUCCESS) {
        ControlTraceW(session, nameA, nullptr, EVENT_TRACE_CONTROL_STOP);
    }

    rc = tryStart(nameB, EVENT_TRACE_REAL_TIME_MODE, 1, 2, &session);
    std::printf("B. virgin name, 1/2 buffers (minimal)        : %lu %s\n", rc, explain(rc));
    if (rc == ERROR_SUCCESS) {
        ControlTraceW(session, nameB, nullptr, EVENT_TRACE_CONTROL_STOP);
    }

    rc = tryStart(nameC, EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_USE_PAGED_MEMORY,
                  4, 16, &session);
    std::printf("C. virgin name, + USE_PAGED_MEMORY           : %lu %s\n", rc, explain(rc));
    if (rc == ERROR_SUCCESS) {
        ControlTraceW(session, nameC, nullptr, EVENT_TRACE_CONTROL_STOP);
    }

    rc = tryStart(L"Flowbit-AppMonitor", EVENT_TRACE_REAL_TIME_MODE, 4, 16, &session);
    std::printf("D. the LEGACY name (known to exist)          : %lu %s\n", rc, explain(rc));

    rc = tryStart(nameE, EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_PRIVATE_LOGGER_MODE,
                  4, 16, &session);
    std::printf("E. virgin name, PRIVATE_LOGGER_MODE          : %lu %s\n", rc, explain(rc));
    if (rc == ERROR_SUCCESS) {
        std::printf("   -> a PRIVATE logger works: per-process sessions are not blocked\n");
        ControlTraceW(session, nameE, nullptr, EVENT_TRACE_CONTROL_STOP);
    }

    // Can the existing legacy session be stopped at all?
    {
        const ULONG bufSize = sizeof(EVENT_TRACE_PROPERTIES) + 2048 * sizeof(wchar_t);
        std::vector<BYTE> buf(bufSize, 0);
        auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
        props->Wnode.BufferSize = bufSize;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        const ULONG src = ControlTraceW(0, L"Flowbit-AppMonitor", props,
                                        EVENT_TRACE_CONTROL_STOP);
        std::printf("\nSTOP the legacy session : %lu %s\n", src, explain(src));
    }

    std::printf("\n=== done ===\n");
    return 0;
}
