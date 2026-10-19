// Asks Windows for the authoritative schema of every Microsoft-Windows-Kernel-Network
// event, using the same TDH API the app uses at runtime.
//
// Why this exists: the app's decoder was written against assumptions about which
// field holds the byte count and how direction is encoded. Those assumptions were
// wrong on Windows build 26300 (only 40% of events decoded, and download/upload
// came out inverted). This tool prints what the OS actually publishes - event id,
// version, opcode, level, and every payload property with its declared type - so
// the decoder can be written against facts.
//
// Run elevated. No session is needed for TDH metadata; it is read from the
// provider registration.

#include <winsock2.h>
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#include <cstdio>
#include <string>
#include <vector>

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

const GUID kProvider = {0x7dd42a49, 0x5329, 0x4832,
                        {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};

// TDH in-type -> readable name. Values are the stable ABI numbers from the
// Windows SDK (MinGW's tdh.h omits the enum, which is itself a useful fact).
const char *inTypeName(USHORT t)
{
    switch (t) {
    case 0: return "Null";
    case 1: return "UnicodeString";
    case 2: return "AnsiString";
    case 3: return "Int8";
    case 4: return "UInt8";
    case 5: return "Int16";
    case 6: return "UInt16";
    case 7: return "Int32";
    case 8: return "UInt32";
    case 9: return "Int64";
    case 10: return "UInt64";
    case 11: return "Float";
    case 12: return "Double";
    case 13: return "Boolean";
    case 14: return "Binary";
    case 15: return "Guid";
    case 16: return "Pointer";
    case 17: return "FileTime";
    case 18: return "SystemTime";
    case 19: return "Sid";
    case 20: return "HexInt32";
    case 21: return "HexInt64";
    default: return "?";
    }
}

const char *outTypeName(USHORT t)
{
    switch (t) {
    case 0: return "Null";
    case 1: return "String";
    case 2: return "DateTime";
    case 3: return "Boolean";
    case 4: return "Hex";
    case 5: return "UnsignedByte";
    case 6: return "UnsignedShort";
    case 7: return "UnsignedInt";
    case 8: return "UnsignedLong";
    case 10: return "SignedByte";
    case 11: return "SignedShort";
    case 12: return "SignedInt";
    case 13: return "SignedLong";
    case 14: return "Float";
    case 15: return "Double";
    case 16: return "Char";
    case 20: return "Guid";
    case 21: return "SizeT";
    case 22: return "HexInt32";
    case 23: return "HexInt64";
    case 33: return "Pid";
    case 34: return "Tid";
    case 35: return "IPv4";
    case 36: return "IPv6";
    case 37: return "SocketAddress";
    case 38: return "CimDateTime";
    case 42: return "ErrorCode";
    default: return "?";
    }
}

// One event's metadata, as published by the provider.
void dumpEvent(ULONG eventId)
{
    // TDH has no "enumerate an event by id" entry point; the supported route is
    // to build a synthetic EVENT_RECORD carrying the provider GUID + event id and
    // let TdhGetEventInformation resolve it from the registered manifest.
    EVENT_DESCRIPTOR desc{};
    desc.Id = USHORT(eventId);
    desc.Version = 0;
    desc.Channel = 0;
    desc.Level = 0;
    desc.Opcode = 0;
    desc.Task = 0;
    desc.Keyword = 0;

    // Ask for the maximum version Windows publishes for this id.
    for (USHORT version = 0; version < 8; ++version) {
        desc.Version = version;

        EVENT_RECORD record{};
        record.EventHeader.Size = sizeof(EVENT_HEADER);
        record.EventHeader.ProviderId = kProvider;
        record.EventHeader.EventDescriptor = desc;
        record.EventHeader.Flags = 0;
        record.EventHeader.ProcessId = 0;
        record.UserDataLength = 0;
        record.UserData = nullptr;

        ULONG size = 0;
        ULONG rc = TdhGetEventInformation(&record, 0, nullptr, nullptr, &size);
        if (rc != ERROR_INSUFFICIENT_BUFFER)
            continue;

        std::vector<BYTE> buf(size);
        auto *info = reinterpret_cast<PTRACE_EVENT_INFO>(buf.data());
        rc = TdhGetEventInformation(&record, 0, nullptr, info, &size);
        if (rc != ERROR_SUCCESS)
            continue;

        out("");
        out("  EVENT id=%u version=%u  task=%u opcode=%u level=%u  properties=%lu",
            unsigned(eventId), unsigned(version),
            unsigned(info->EventDescriptor.Task),
            unsigned(info->EventDescriptor.Opcode),
            unsigned(info->EventDescriptor.Level),
            static_cast<unsigned long>(info->TopLevelPropertyCount));
        if (info->ProviderNameOffset)
            out("    provider: %S",
                reinterpret_cast<wchar_t *>(buf.data() + info->ProviderNameOffset));
        if (info->TaskNameOffset)
            out("    task    : %S",
                reinterpret_cast<wchar_t *>(buf.data() + info->TaskNameOffset));
        if (info->OpcodeNameOffset)
            out("    opcode  : %S",
                reinterpret_cast<wchar_t *>(buf.data() + info->OpcodeNameOffset));

        for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
            const EVENT_PROPERTY_INFO &p = info->EventPropertyInfoArray[i];
            const wchar_t *nm = p.NameOffset
                ? reinterpret_cast<const wchar_t *>(buf.data() + p.NameOffset)
                : L"(unnamed)";
            const bool isStruct = (p.Flags & 0x1) != 0;
            if (isStruct) {
                out("    prop[%lu] %-12S  STRUCT members=%u",
                    static_cast<unsigned long>(i), nm,
                    unsigned(p.structType.NumOfStructMembers));
            } else {
                out("    prop[%lu] %-12S  in=%-14s out=%-14s count=%u flags=0x%x",
                    static_cast<unsigned long>(i), nm,
                    inTypeName(p.nonStructType.InType),
                    outTypeName(p.nonStructType.OutType),
                    unsigned(p.count), unsigned(p.Flags));
            }
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *logPath = (argc > 1) ? argv[1] : "kernel_network_schema.log";
    g_log = fopen(logPath, "w");
    if (!g_log)
        return 2;

    out("=== Microsoft-Windows-Kernel-Network published schema ===");
    out("provider GUID: {7dd42a49-5329-4832-8dfd-43d979153a88}");

    for (ULONG id = 0; id <= 32; ++id)
        dumpEvent(id);

    out("");
    out("=== done ===");
    fclose(g_log);
    return 0;
}
