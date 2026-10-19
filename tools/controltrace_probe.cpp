// Verifies what ControlTraceW actually returns for the monitoring session name.
//
// The app's leftover-session check reported a "ghost session" that logman could
// not see and that survived every stop attempt. This prints the raw return codes
// for QUERY and STOP so the check can be written against observed behaviour
// instead of assumptions.

#include <windows.h>
#include <evntrace.h>

#include <cstdio>
#include <vector>

namespace {

void probe(FILE *f, const wchar_t *name)
{
    const ULONG bufSize = sizeof(EVENT_TRACE_PROPERTIES) + 2048 * sizeof(wchar_t);

    // QUERY
    {
        std::vector<BYTE> buf(bufSize, 0);
        auto *p = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
        p->Wnode.BufferSize = bufSize;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        const ULONG rc = ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_QUERY);
        fprintf(f, "  QUERY  '%S' -> %lu\n", name, static_cast<unsigned long>(rc));
    }

    // QUERY without LoggerNameOffset (the shape the app used before)
    {
        std::vector<BYTE> buf(bufSize, 0);
        auto *p = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
        p->Wnode.BufferSize = bufSize;
        const ULONG rc = ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_QUERY);
        fprintf(f, "  QUERY  '%S' (no LoggerNameOffset) -> %lu\n",
                name, static_cast<unsigned long>(rc));
    }

    // STOP
    {
        std::vector<BYTE> buf(bufSize, 0);
        auto *p = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
        p->Wnode.BufferSize = bufSize;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        const ULONG rc = ControlTraceW(0, name, p, EVENT_TRACE_CONTROL_STOP);
        fprintf(f, "  STOP   '%S' -> %lu\n", name, static_cast<unsigned long>(rc));
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "controltrace_probe.log";
    FILE *f = fopen(path, "w");
    if (!f)
        return 2;

    fprintf(f, "=== ControlTraceW behaviour probe ===\n");
    fprintf(f, "reference codes: 0=SUCCESS  4201=ERROR_WMI_INSTANCE_NOT_FOUND\n");
    fprintf(f, "                 183=ERROR_ALREADY_EXISTS  5=ACCESS_DENIED\n\n");

    fprintf(f, "names that certainly do NOT exist:\n");
    probe(f, L"Flowbit-Definitely-Not-A-Session-12345");
    probe(f, L"Totally-Made-Up-Session-Name-98765");

    fprintf(f, "\nthe monitoring session name:\n");
    probe(f, L"Flowbit-AppMonitor");

    fprintf(f, "\n=== done ===\n");
    fclose(f);
    return 0;
}
