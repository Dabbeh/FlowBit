// Prints the Microsoft-Windows-Kernel-Network event schema straight from the provider's
// registered manifest, with NO ETW session and NO Administrator rights.
//
// WHY THIS EXISTS
//
// docs/etw-kernel-network-schema.md says the schema must be re-derived with
// tools/etw_schema_dump.cpp, which needs an elevated session. That is true for a live dump, but
// it is not the only way: TdhGetManifestEventInformation reads the manifest Windows already has
// registered, and needs no session at all. So the two questions that blocked the decoder can be
// answered from an ordinary shell.
//
// QUESTIONS THIS SETTLES
//
//   1. Do the UDP events carry a `size` property? src/appmonitor.cpp reads `size` by name, so
//      the UDP ids 42/43/58/59 only yield bytes if they publish it. ANSWER: yes, all four do,
//      with the same eight properties as the TCP ids.
//
//   2. Does id 18 carry bytes? The schema doc records it as "TCP ... " with an unknown meaning.
//      ANSWER: it has `size` and the same layout as the receive/retransmit ids, so it does
//      carry bytes - but see the note at the bottom before counting it.
//
// Build:
//   g++ -std=gnu++17 -O1 -DWIN32 -DUNICODE -D_UNICODE tools/etw_manifest_probe.cpp -o etw_manifest_probe.exe
// Run: no arguments, no elevation.
//
// The API is resolved through GetProcAddress because the MinGW tdh.h shipped with the Qt
// toolchain predates TdhGetManifestEventInformation.

#include <windows.h>
#include <tdh.h>

#include <cstdio>
#include <vector>

namespace {

const GUID kKernelNetwork = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};

using PFN_ManifestInfo = ULONG(WINAPI *)(LPCGUID, PEVENT_DESCRIPTOR, PTRACE_EVENT_INFO,
                                         PULONG);

PFN_ManifestInfo g_getManifestInfo = nullptr;

const wchar_t *nameAt(const std::vector<BYTE> &buffer, ULONG offset)
{
    if (offset == 0 || offset >= buffer.size())
        return L"";
    return reinterpret_cast<const wchar_t *>(buffer.data() + offset);
}

void dump(USHORT id)
{
    EVENT_DESCRIPTOR desc{};
    desc.Id = id;

    ULONG size = 0;
    ULONG rc = g_getManifestInfo(&kKernelNetwork, &desc, nullptr, &size);
    if (rc != ERROR_INSUFFICIENT_BUFFER) {
        std::printf("id %-3u  not in manifest (rc=%lu)\n", id, rc);
        return;
    }

    std::vector<BYTE> buffer(size);
    auto *info = reinterpret_cast<PTRACE_EVENT_INFO>(buffer.data());
    rc = g_getManifestInfo(&kKernelNetwork, &desc, info, &size);
    if (rc != ERROR_SUCCESS) {
        std::printf("id %-3u  read failed (rc=%lu)\n", id, rc);
        return;
    }

    std::printf("id %-3u  task=%-18ls opcode=%-16ls props=%-2u :", id,
                nameAt(buffer, info->TaskNameOffset), nameAt(buffer, info->OpcodeNameOffset),
                info->TopLevelPropertyCount);

    bool hasSize = false;
    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const EVENT_PROPERTY_INFO &prop = info->EventPropertyInfoArray[i];
        const wchar_t *propName =
            reinterpret_cast<const wchar_t *>(buffer.data() + prop.NameOffset);
        std::printf(" %ls", propName);
        if (_wcsicmp(propName, L"size") == 0)
            hasSize = true;
    }
    std::printf("   -> %s\n", hasSize ? "HAS size" : "no size");
}

} // namespace

int main()
{
    HMODULE tdh = LoadLibraryW(L"tdh.dll");
    if (!tdh) {
        std::printf("could not load tdh.dll\n");
        return 2;
    }

    g_getManifestInfo = reinterpret_cast<PFN_ManifestInfo>(
        reinterpret_cast<void *>(GetProcAddress(tdh, "TdhGetManifestEventInformation")));
    if (!g_getManifestInfo) {
        std::printf("TdhGetManifestEventInformation is not available in tdh.dll\n");
        return 2;
    }

    std::printf("Microsoft-Windows-Kernel-Network provider manifest\n");
    std::printf("(no ETW session, no elevation required)\n\n");

    const USHORT ids[] = {
        10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        26, 27, 28, 29, 30, 31, 32,
        42, 43, 44, 45,
        58, 59, 60, 61,
    };

    for (USHORT id : ids) {
        dump(id);
        std::printf("\n");
    }

    std::printf(
        "NOTE on id 18: it publishes `size` and carries bytes, but it is deliberately NOT\n"
        "counted. It has the same layout as the receive and retransmit ids, so counting it\n"
        "alongside 10/11 risks counting the same bytes twice, and that is worse than\n"
        "under-counting. Settling it needs a live capture: compare the byte total of id 10 + 11\n"
        "against id 18 over the same interval, with an idle-only vs active download.\n");

    return 0;
}
