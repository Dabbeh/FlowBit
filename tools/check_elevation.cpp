// Reports the integrity level of a running process by PID.
//
// Written because the elevation question has now caused two wrong diagnoses in a
// row, and a PowerShell one-liner with inline C# failed on quoting - an unreliable
// answer is worse than none. This is compiled once and gives a plain verdict.
//
//   Elevated (High integrity)     -> ETW session creation is permitted
//   Not elevated (Medium)         -> every StartTrace answers ACCESS_DENIED

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::printf("usage: check_elevation.exe <pid>\n");
        return 2;
    }
    const DWORD pid = DWORD(std::strtoul(argv[1], nullptr, 10));

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        std::printf("cannot open pid %lu (error %lu)\n", pid, GetLastError());
        return 1;
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        std::printf("cannot open token (error %lu)\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    int level = -1;
    if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(), size, &size)) {
        auto *tml = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf.data());
        DWORD count = *GetSidSubAuthorityCount(tml->Label.Sid);
        level = int(*GetSidSubAuthority(tml->Label.Sid, count - 1));
    }

    // TokenElevation is the authoritative "is this process elevated" answer.
    TOKEN_ELEVATION elev{};
    DWORD esize = sizeof(elev);
    const bool elevated = GetTokenInformation(token, TokenElevation, &elev, esize, &esize)
                          && elev.TokenIsElevated;

    std::printf("pid %lu\n", pid);
    std::printf("  integrity level : 0x%X (%s)\n", level,
                level >= 0x3000 ? "High" : (level >= 0x2000 ? "Medium" : "Low"));
    std::printf("  TokenElevation  : %s\n", elevated ? "ELEVATED" : "NOT elevated");
    std::printf("  => ETW sessions : %s\n",
                elevated ? "can be created" : "will be DENIED (ACCESS_DENIED)");

    CloseHandle(token);
    CloseHandle(proc);
    return elevated ? 0 : 1;
}
