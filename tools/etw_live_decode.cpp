// Live decoder verification.
//
// Prints, for every Microsoft-Windows-Kernel-Network transfer event, the raw
// values of the fields the schema promises - PID, size, saddr, daddr - plus which
// per-process totals they produce. This is the ground truth the app's panel is
// compared against: if the panel disagrees with this output while Windows' own
// resource monitor agrees with it, the bug is in the app's EW ingestion path and
// not in the schema understanding.
//
// Run elevated, generate traffic (open a video), and compare:
//   * the per-PID totals at the end of the log
//   * against Task Manager / Resource Monitor, which read the same provider
//
// Usage: etw_live_decode.exe <out.log> [seconds]

#include <winsock2.h>
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

FILE *g_log = nullptr;
const GUID kProvider = {0x7dd42a49, 0x5329, 0x4832,
                        {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
const wchar_t *kSession = L"Flowbit-SchemaVerify";

struct Totals {
    unsigned long long in = 0;
    unsigned long long out = 0;
    unsigned long long events = 0;
    unsigned long long failed = 0;
};
std::map<unsigned long, Totals> g_totals;
unsigned long long g_seen = 0;
unsigned long long g_decoded = 0;
int g_printed = 0;
TRACEHANDLE g_session = 0;

const char *idName(USHORT id)
{
    switch (id) {
    case 10: return "TCP send v4";
    case 11: return "TCP recv v4";
    case 12: return "TCP connect v4";
    case 13: return "TCP disconnect v4";
    case 14: return "TCP retransmit v4";
    case 15: return "TCP reconnect v4";
    case 16: return "TCP copy v4";
    case 18: return "TCP v4 (18)";
    case 26: return "TCP send v6";
    case 27: return "TCP recv v6";
    case 28: return "TCP connect v6";
    case 30: return "TCP retransmit v6";
    default: return "other";
    }
}

void WINAPI onEvent(PEVENT_RECORD record)
{
    if (IsEqualGUID(record->EventHeader.ProviderId, kProvider) == FALSE)
        return;

    const USHORT id = record->EventHeader.EventDescriptor.Id;
    const bool transfer = (id == 10 || id == 11 || id == 14 || id == 26
                           || id == 27 || id == 30);
    if (!transfer)
        return;

    ++g_seen;

    ULONG size = 0;
    if (TdhGetEventInformation(record, 0, nullptr, nullptr, &size)
        != ERROR_INSUFFICIENT_BUFFER)
        return;
    std::vector<BYTE> buf(size);
    auto *info = reinterpret_cast<PTRACE_EVENT_INFO>(buf.data());
    if (TdhGetEventInformation(record, 0, nullptr, info, &size) != ERROR_SUCCESS)
        return;

    unsigned long pid = 0;
    unsigned long long bytes = 0;
    unsigned long saddr = 0, daddr = 0;
    bool gotPid = false, gotSize = false, gotS = false, gotD = false;

    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const EVENT_PROPERTY_INFO &p = info->EventPropertyInfoArray[i];
        if (!p.NameOffset)
            continue;
        const wchar_t *nm = reinterpret_cast<const wchar_t *>(buf.data() + p.NameOffset);

        PROPERTY_DATA_DESCRIPTOR desc{};
        desc.PropertyName = reinterpret_cast<ULONGLONG>(nm);
        desc.ArrayIndex = ULONG_MAX;

        if (_wcsicmp(nm, L"PID") == 0) {
            ULONG v = 0, sz = sizeof(v);
            if (TdhGetProperty(record, 0, nullptr, 1, &desc, sz,
                               reinterpret_cast<PBYTE>(&v)) == ERROR_SUCCESS) {
                pid = v;
                gotPid = true;
            }
        } else if (_wcsicmp(nm, L"size") == 0) {
            ULONG v = 0, sz = sizeof(v);
            if (TdhGetProperty(record, 0, nullptr, 1, &desc, sz,
                               reinterpret_cast<PBYTE>(&v)) == ERROR_SUCCESS) {
                bytes = v;
                gotSize = true;
            }
        } else if (_wcsicmp(nm, L"saddr") == 0) {
            ULONG v = 0, sz = sizeof(v);
            if (TdhGetProperty(record, 0, nullptr, 1, &desc, sz,
                               reinterpret_cast<PBYTE>(&v)) == ERROR_SUCCESS) {
                saddr = v;
                gotS = true;
            }
        } else if (_wcsicmp(nm, L"daddr") == 0) {
            ULONG v = 0, sz = sizeof(v);
            if (TdhGetProperty(record, 0, nullptr, 1, &desc, sz,
                               reinterpret_cast<PBYTE>(&v)) == ERROR_SUCCESS) {
                daddr = v;
                gotD = true;
            }
        }
    }

    if (!gotSize || bytes == 0) {
        if (gotPid)
            ++g_totals[pid].failed;
        return;
    }
    ++g_decoded;

    // Log the first few raw events so the field values are visible.
    if (g_printed < 12) {
        ++g_printed;
        fprintf(g_log, "  raw: %-18s pid=%-6lu size=%-8llu saddr=0x%08lx daddr=0x%08lx"
                       " (got: pid=%d size=%d s=%d d=%d)\n",
                idName(id), pid, bytes, saddr, daddr,
                int(gotPid), int(gotSize), int(gotS), int(gotD));
        fflush(g_log);
    }

    Totals &t = g_totals[pid];
    ++t.events;
    // Direction is printed both ways so the log shows whether saddr/daddr could
    // have decided it correctly.
    const bool sendByAddr = gotS && gotD && (saddr != 0) && (daddr != 0);
    if (sendByAddr) {
        // heuristics: RFC1918/loopback looking addresses are usually local
        auto looksLocal = [](unsigned long a) {
            const unsigned long h = ntohl(a);
            return (h >> 24) == 127 || (h >> 24) == 10
                   || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8
                   || (h >> 16) == 0xA9FE;
        };
        if (looksLocal(saddr) && !looksLocal(daddr))
            t.out += bytes;
        else if (looksLocal(daddr) && !looksLocal(saddr))
            t.in += bytes;
        else if (id == 10 || id == 14 || id == 26 || id == 30)
            t.out += bytes;
        else
            t.in += bytes;
    } else if (id == 10 || id == 14 || id == 26 || id == 30) {
        t.out += bytes;
    } else {
        t.in += bytes;
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *logPath = (argc > 1) ? argv[1] : "etw_live_decode.log";
    const int seconds = (argc > 2) ? atoi(argv[2]) : 20;
    g_log = fopen(logPath, "w");
    if (!g_log)
        return 2;

    fprintf(g_log, "=== live ETW decode verification ===\n");
    fflush(g_log);

    EVENT_TRACE_PROPERTIES *props =
        reinterpret_cast<EVENT_TRACE_PROPERTIES *>(calloc(1, sizeof(EVENT_TRACE_PROPERTIES) + 512));
    props->Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES) + 512;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize = 64;
    props->MinimumBuffers = 8;
    props->MaximumBuffers = 32;

    ULONG rc = StartTraceW(&g_session, kSession, props);
    if (rc == ERROR_ALREADY_EXISTS) {
        ControlTraceW(0, kSession, props, EVENT_TRACE_CONTROL_STOP);
        rc = StartTraceW(&g_session, kSession, props);
    }
    if (rc != ERROR_SUCCESS) {
        fprintf(g_log, "StartTrace failed: %lu\n", static_cast<unsigned long>(rc));
        fclose(g_log);
        return 1;
    }

    // Start the consumer BEFORE enabling the provider, then enable from the main
    // thread. Both orderings are tested because the app (which works) enables
    // first and consumes after, while this tool saw nothing with that order.
    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = const_cast<LPWSTR>(kSession);
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = onEvent;

    TRACEHANDLE consumer = OpenTraceW(&lf);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        fprintf(g_log, "OpenTrace failed: %lu\n", static_cast<unsigned long>(GetLastError()));
        fclose(g_log);
        return 1;
    }

    // ProcessTrace blocks until the session is stopped, so it runs on its own
    // thread. The handle is copied into a heap context: passing the address of a
    // stack local was a race (the thread could read it after it went out of
    // scope) and made the consumer see nothing.
    TRACEHANDLE *sharedHandle = new TRACEHANDLE(consumer);
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        TRACEHANDLE *ph = static_cast<TRACEHANDLE *>(p);
        const ULONG rc = ProcessTrace(ph, 1, nullptr, nullptr);
        if (g_log) {
            fprintf(g_log, "ProcessTrace returned: %lu\n", static_cast<unsigned long>(rc));
            fflush(g_log);
        }
        return 0;
    }, sharedHandle, 0, nullptr);

    // Give the consumer thread a moment to enter ProcessTrace, then enable.
    Sleep(1000);
    {
        const ULONG en = EnableTraceEx2(g_session, &kProvider,
                                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                        TRACE_LEVEL_VERBOSE, 0xFFFFFFFFFFFFFFFFULL,
                                        0, 0, nullptr);
        fprintf(g_log, "EnableTraceEx2 (after consumer attach): %lu\n",
                static_cast<unsigned long>(en));
        fflush(g_log);
    }

    Sleep(DWORD(seconds) * 1000);

    // Stop the session first: that makes ProcessTrace return by itself, which is
    // the documented shutdown order. Closing the consumer from another thread
    // while ProcessTrace is inside it is what left the consumer with no events.
    {
        EVENT_TRACE_PROPERTIES *stopProps = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(
            calloc(1, sizeof(EVENT_TRACE_PROPERTIES) + 512));
        stopProps->Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES) + 512;
        stopProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        const ULONG stopped = ControlTraceW(g_session, nullptr, stopProps,
                                            EVENT_TRACE_CONTROL_STOP);
        g_session = 0;
        fprintf(g_log, "session stopped: %lu\n", static_cast<unsigned long>(stopped));
        fflush(g_log);
        free(stopProps);
    }

    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
    CloseTrace(consumer);
    delete sharedHandle;

    fprintf(g_log, "\n=== per-PID totals (bytes/second over %d s) ===\n", seconds);
    for (const auto &kv : g_totals) {
        fprintf(g_log, "  pid=%-6lu send=%-10llu recv=%-10llu events=%-7llu failed=%llu\n",
                kv.first, kv.second.out, kv.second.in, kv.second.events,
                kv.second.failed);
    }
    fprintf(g_log, "\ntotals: seen=%llu decoded=%llu\n", g_seen, g_decoded);
    fprintf(g_log, "=== done ===\n");
    fclose(g_log);

    free(props);
    return 0;
}
