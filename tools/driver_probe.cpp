// Driver + ETW probe.
//
// Purpose: find out empirically why WinDivertOpen() reports
// ERROR_INVALID_PARAMETER (87) for the App Limiter's handles, and what the
// kernel-network ETW provider is actually handing us on this Windows build.
//
// It tries a matrix of (layer, priority, filter) combinations against the live
// driver and prints the raw result of each, then consumes the ETW session for a
// few seconds and prints a per-event-ID histogram plus the property names of the
// first event of each ID. Nothing is guessed: every line is a measurement.
//
// Run elevated (the ETW session and the driver's flow layer need it).

#include <winsock2.h>
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../third_party/WinDivert/WinDivert-2.2.2-A/include/windivert.h"

namespace {

FILE *g_log = nullptr;

void out(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

const char *layerName(WINDIVERT_LAYER layer)
{
    switch (layer) {
    case WINDIVERT_LAYER_NETWORK: return "NETWORK";
    case WINDIVERT_LAYER_NETWORK_FORWARD: return "NETWORK_FORWARD";
    case WINDIVERT_LAYER_FLOW: return "FLOW";
    case WINDIVERT_LAYER_SOCKET: return "SOCKET";
    case WINDIVERT_LAYER_REFLECT: return "REFLECT";
    default: return "?";
    }
}

// One attempt: open, report, close. Kept deliberately dumb so the error code is
// never contaminated by later calls.
void tryOpen(WINDIVERT_LAYER layer, INT16 priority, const char *filter, UINT64 flags)
{
    SetLastError(0);
    HANDLE h = WinDivertOpen(filter, layer, priority, flags);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        out("  OPEN %-15s prio=%-5d filter=\"%s\" flags=0x%llx  -> FAILED err=%lu",
            layerName(layer), int(priority), filter,
            static_cast<unsigned long long>(flags),
            static_cast<unsigned long>(err));
        return;
    }
    out("  OPEN %-15s prio=%-5d filter=\"%s\" flags=0x%llx  -> OK",
        layerName(layer), int(priority), filter,
        static_cast<unsigned long long>(flags));
    WinDivertClose(h);
}

// Filter compilation, independent of the driver: separates "the filter text is
// rejected by the compiler" from "the driver rejected the handle".
void tryCompile(WINDIVERT_LAYER layer, const char *filter)
{
    UINT32 errorPos = 0;
    const char *errorStr = nullptr;
    const BOOL ok = WinDivertHelperCompileFilter(filter, layer, nullptr, 0,
                                                 &errorStr, &errorPos);
    out("  COMPILE %-13s filter=\"%s\" -> %s%s%s", layerName(layer), filter,
        ok ? "ok" : "REJECTED",
        (ok || !errorStr) ? "" : " : ", (ok || !errorStr) ? "" : errorStr);
}

// ---------------------------------------------------------------- ETW side ---

struct IdStat {
    unsigned long long seen = 0;
    unsigned long long decoded = 0;
    bool dumpedProps = false;
};

std::map<int, IdStat> g_ids;
TRACEHANDLE g_session = 0;
bool g_sessionOwned = false;

const GUID kProvider = {0x7dd42a49, 0x4309, 0x4832,
                        {0x8d, 0xfd, 0xe3, 0x64, 0xf5, 0x5c, 0xa9, 0x90}};
const wchar_t *kSessionName = L"Flowbit-Probe-Session";

void dumpProps(PEVENT_RECORD record, int id)
{
    ULONG size = 0;
    if (TdhGetEventInformation(record, 0, nullptr, nullptr, &size) != ERROR_INSUFFICIENT_BUFFER)
        return;
    std::vector<BYTE> buf(size);
    auto *info = reinterpret_cast<PTRACE_EVENT_INFO>(buf.data());
    if (TdhGetEventInformation(record, 0, nullptr, info, &size) != ERROR_SUCCESS)
        return;

    out("    [id %d] opcode=%u level=%u version=%u",
        id, unsigned(record->EventHeader.EventDescriptor.Opcode),
        unsigned(record->EventHeader.EventDescriptor.Level),
        unsigned(record->EventHeader.EventDescriptor.Version));
    out("    [id %d] top-level properties: %lu", id,
        static_cast<unsigned long>(info->TopLevelPropertyCount));
    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const auto &p = info->EventPropertyInfoArray[i];
        const wchar_t *nm = reinterpret_cast<const wchar_t *>(
            buf.data() + p.NameOffset);
        out("    [id %d]   prop[%lu] '%S' count=%u flags=0x%x",
            id, static_cast<unsigned long>(i), nm,
            unsigned(p.count), unsigned(p.Flags));
    }
}

void WINAPI onEvent(PEVENT_RECORD record)
{
    if (IsEqualGUID(record->EventHeader.ProviderId, kProvider) == FALSE)
        return;

    const int id = int(record->EventHeader.EventDescriptor.Id);
    IdStat &st = g_ids[id];
    ++st.seen;

    if (!st.dumpedProps) {
        st.dumpedProps = true;
        dumpProps(record, id);
    }

    // Same decode strategy as the app: find a top-level 'size' property.
    ULONG size = 0;
    if (TdhGetEventInformation(record, 0, nullptr, nullptr, &size)
        != ERROR_INSUFFICIENT_BUFFER)
        return;
    std::vector<BYTE> buf(size);
    auto *info = reinterpret_cast<PTRACE_EVENT_INFO>(buf.data());
    if (TdhGetEventInformation(record, 0, nullptr, info, &size) != ERROR_SUCCESS)
        return;

    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const wchar_t *nm = reinterpret_cast<const wchar_t *>(
            buf.data() + info->EventPropertyInfoArray[i].NameOffset);
        if (_wcsicmp(nm, L"size") != 0)
            continue;
        PROPERTY_DATA_DESCRIPTOR desc{};
        desc.PropertyName = reinterpret_cast<ULONGLONG>(nm);
        desc.ArrayIndex = ULONG_MAX;
        ULONG value = 0;
        ULONG vsz = sizeof(value);
        if (TdhGetProperty(record, 0, nullptr, 1, &desc, vsz,
                           reinterpret_cast<PBYTE>(&value)) == ERROR_SUCCESS
            && value > 0) {
            ++st.decoded;
        }
        break;
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *logPath = (argc > 1) ? argv[1] : "driver_probe.log";
    g_log = fopen(logPath, "w");
    if (!g_log)
        return 2;

    out("=== driver probe ===");
    out("windivert DLL loaded");

    out("");
    out("1) filter compilation (no driver involved)");
    tryCompile(WINDIVERT_LAYER_NETWORK, "true");
    tryCompile(WINDIVERT_LAYER_NETWORK, "inbound and not loopback");
    tryCompile(WINDIVERT_LAYER_FLOW, "true");
    tryCompile(WINDIVERT_LAYER_FLOW, "inbound");
    tryCompile(WINDIVERT_LAYER_FLOW, "event == ESTABLISHED or event == DELETED");

    out("");
    out("2) handle opens, isolated (nothing else open)");
    tryOpen(WINDIVERT_LAYER_FLOW, 0, "true", 0);
    tryOpen(WINDIVERT_LAYER_FLOW, 100, "true", 0);
    tryOpen(WINDIVERT_LAYER_NETWORK, 0, "true", 0);
    tryOpen(WINDIVERT_LAYER_NETWORK, 100, "true", 0);

    out("");
    out("2b) SOCKET layer - the attribution fallback when FLOW is unavailable");
    tryCompile(WINDIVERT_LAYER_SOCKET, "true");
    tryCompile(WINDIVERT_LAYER_SOCKET, "event == CONNECT");
    tryOpen(WINDIVERT_LAYER_SOCKET, 0, "true", 0);
    tryOpen(WINDIVERT_LAYER_SOCKET, 100, "true", 0);
    tryOpen(WINDIVERT_LAYER_SOCKET, 0, "event == CONNECT", 0);
    tryOpen(WINDIVERT_LAYER_SOCKET, 0, "event == CONNECT or event == CLOSE", 0);

    out("");
    out("3) opens in the order the App Limiter uses them");
    {
        HANDLE flow = WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 100, 0);
        out("  flow handle: %s", (flow == INVALID_HANDLE_VALUE) ? "FAILED" : "ok");
        if (flow == INVALID_HANDLE_VALUE)
            out("    err=%lu", static_cast<unsigned long>(GetLastError()));
        HANDLE net = WinDivertOpen("true", WINDIVERT_LAYER_NETWORK, 100, 0);
        out("  net handle (while flow open): %s",
            (net == INVALID_HANDLE_VALUE) ? "FAILED" : "ok");
        if (net == INVALID_HANDLE_VALUE)
            out("    err=%lu", static_cast<unsigned long>(GetLastError()));
        if (net != INVALID_HANDLE_VALUE) {
            out("  setparam queue length: %d",
                int(WinDivertSetParam(net, WINDIVERT_PARAM_QUEUE_LENGTH, 8192)));
            out("  setparam queue time:   %d",
                int(WinDivertSetParam(net, WINDIVERT_PARAM_QUEUE_TIME, 2000)));
            WinDivertClose(net);
        }
        if (flow != INVALID_HANDLE_VALUE)
            WinDivertClose(flow);
    }

    out("");
    out("3b) SOCKET layer event sample (does it carry ProcessId?)");
    {
        HANDLE sock = WinDivertOpen("event == CONNECT", WINDIVERT_LAYER_SOCKET, 0, 0);
        if (sock == INVALID_HANDLE_VALUE) {
            out("  socket handle: FAILED err=%lu",
                static_cast<unsigned long>(GetLastError()));
        } else {
            out("  socket handle: ok - sampling 4 seconds");
            std::vector<char> buf(65535);
            const DWORD deadline = GetTickCount() + 4000;
            int shown = 0;
            while (GetTickCount() < deadline && shown < 12) {
                UINT len = 0;
                WINDIVERT_ADDRESS addr{};
                if (!WinDivertRecv(sock, buf.data(), UINT(buf.size()), &len, &addr))
                    continue;
                if (addr.Event == WINDIVERT_EVENT_SOCKET_CONNECT) {
                    out("  CONNECT event: pid=%lu proto=%u local=%s:%u remote=%s:%u",
                        static_cast<unsigned long>(addr.Socket.ProcessId),
                        unsigned(addr.Socket.Protocol),
                        "?", unsigned(addr.Socket.LocalPort),
                        "?", unsigned(addr.Socket.RemotePort));
                    ++shown;
                }
                // Socket events must be re-injected like any other.
                WinDivertSend(sock, buf.data(), len, nullptr, &addr);
            }
            WinDivertClose(sock);
        }
    }

    out("");
    out("4) two handles at the same priority with overlapping filters");
    {
        HANDLE a = WinDivertOpen("true", WINDIVERT_LAYER_NETWORK, 0, 0);
        HANDLE b = WinDivertOpen("true", WINDIVERT_LAYER_NETWORK, 0, 0);
        out("  first: %s   second (same prio, overlapping): %s",
            (a == INVALID_HANDLE_VALUE) ? "FAILED" : "ok",
            (b == INVALID_HANDLE_VALUE) ? "FAILED" : "ok");
        if (b == INVALID_HANDLE_VALUE)
            out("    second err=%lu", static_cast<unsigned long>(GetLastError()));
        if (a != INVALID_HANDLE_VALUE) WinDivertClose(a);
        if (b != INVALID_HANDLE_VALUE) WinDivertClose(b);
    }

    // ------------------------------------------------------------- ETW run ---
    out("");
    out("5) ETW session (same shape as the app)");

    EVENT_TRACE_PROPERTIES *props = nullptr;
    const size_t propsSize = sizeof(EVENT_TRACE_PROPERTIES) + 512;
    props = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(calloc(1, propsSize));
    props->Wnode.BufferSize = ULONG(propsSize);
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize = 64;
    props->MinimumBuffers = 8;
    props->MaximumBuffers = 32;

    ULONG rc = StartTraceW(&g_session, kSessionName, props);
    out("  StartTrace: %lu", static_cast<unsigned long>(rc));
    if (rc == ERROR_ALREADY_EXISTS) {
        out("  (leftover session with this name - stopping it)");
        ControlTraceW(0, kSessionName, nullptr, EVENT_TRACE_CONTROL_QUERY);
        rc = ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
        out("  stop leftover: %lu", static_cast<unsigned long>(rc));
        rc = StartTraceW(&g_session, kSessionName, props);
        out("  StartTrace retry: %lu", static_cast<unsigned long>(rc));
    }
    if (rc != ERROR_SUCCESS) {
        out("  cannot start a session - ETW part ends here");
    } else {
        g_sessionOwned = true;
        const ULONG en = EnableTraceEx2(g_session, &kProvider,
                                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                        TRACE_LEVEL_VERBOSE, 0xFFFFFFFFFFFFFFFFULL,
                                        0, 0, nullptr);
        out("  EnableTraceEx2 (VERBOSE, all keywords): %lu",
            static_cast<unsigned long>(en));

        EVENT_TRACE_LOGFILEW lf{};
        lf.LoggerName = const_cast<LPWSTR>(kSessionName);
        lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME
                              | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = onEvent;

        TRACEHANDLE consumer = OpenTraceW(&lf);
        out("  OpenTrace: %s", (consumer == INVALID_PROCESSTRACE_HANDLE) ? "FAILED" : "ok");
        if (consumer != INVALID_PROCESSTRACE_HANDLE) {
            out("  consuming for 8 seconds - generate some traffic now...");
            // ProcessTrace blocks; run it on a thread and wake it via the
            // session stop after the window.
            HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                ProcessTrace(static_cast<PTRACEHANDLE>(p), 1, nullptr, nullptr);
                return 0;
            }, &consumer, 0, nullptr);
            Sleep(8000);
            CloseTrace(consumer);
            if (th) {
                WaitForSingleObject(th, 3000);
                CloseHandle(th);
            }
        }
    }

    out("");
    out("6) ETW event-id histogram");
    if (g_ids.empty()) {
        out("  NO EVENTS AT ALL from this provider");
    } else {
        for (const auto &kv : g_ids) {
            out("  id %-5d seen=%-8llu decoded=%-8llu", kv.first,
                kv.second.seen, kv.second.decoded);
        }
    }

    if (g_sessionOwned) {
        EVENT_TRACE_PROPERTIES *p2 = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(
            calloc(1, propsSize));
        p2->Wnode.BufferSize = ULONG(propsSize);
        p2->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        const ULONG stopped = ControlTraceW(g_session, nullptr, p2,
                                            EVENT_TRACE_CONTROL_STOP);
        out("  session stopped: %lu", static_cast<unsigned long>(stopped));
        free(p2);
    }
    free(props);

    out("");
    out("=== probe complete ===");
    fclose(g_log);
    return 0;
}
