// Captures OutputDebugString output from any process, so a GUI application's qInfo/qWarning
// lines can be read without a debugger attached.
//
// WHY THIS EXISTS
//
// Flowbit reports every failure on the ETW path through qWarning/qInfo: the provider enable's
// error code, whether the consumer returned, why a session could not be created. The
// application is a WIN32 GUI target with no console, so those lines go to the debugger only and
// vanish in normal use.
//
// That left "ETW events received: 0" with no evidence behind it. The diagnostics row named
// "Leftover trace session" reads an in-process flag rather than querying the session, so it says
// "active (this session)" whether or not a session exists. Every conclusion drawn from that page
// alone was a guess. This tool prints the actual log instead.
//
// Build:
//   g++ -std=gnu++17 -O1 tools/debug_output_capture.cpp -o debug_output_capture.exe
// Run (no elevation needed; it reads the global DBWIN buffer):
//   debug_output_capture.exe 120        # capture for two minutes
//   start Flowbit while it is running

#include <windows.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 60;

    // The classic DBWIN protocol: a shared 4 KB page, with the writing process's id in the
    // first four bytes and an ANSI string after it. A single listener may attach at a time.
    HANDLE bufferReady = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_BUFFER_READY");
    HANDLE dataReady = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_DATA_READY");
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096,
                                        L"DBWIN_BUFFER");

    if (!bufferReady || !dataReady || !mapping) {
        std::printf("Could not open the debug buffer (is another viewer running?)\n");
        return 2;
    }

    auto *buffer = static_cast<BYTE *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 4096));
    if (!buffer) {
        std::printf("Could not map the debug buffer\n");
        return 2;
    }

    std::printf("Capturing OutputDebugString for %d seconds. Start Flowbit now.\n", seconds);
    std::fflush(stdout);

    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG(seconds) * 1000);

    while (GetTickCount64() < deadline) {
        SetEvent(bufferReady);

        const ULONGLONG left = deadline - GetTickCount64();
        const DWORD wait = WaitForSingleObject(dataReady, DWORD(left < 1000 ? left : 1000));
        if (wait != WAIT_OBJECT_0) {
            if (wait == WAIT_FAILED) {
                std::printf("debug buffer wait failed (%lu)\n",
                            static_cast<unsigned long>(GetLastError()));
                break;
            }
            continue;
        }

        const DWORD pid = *reinterpret_cast<const DWORD *>(buffer);
        const char *text = reinterpret_cast<const char *>(buffer + 4);
        std::printf("[pid %5lu] %s\n", static_cast<unsigned long>(pid), text);
        std::fflush(stdout);
    }

    UnmapViewOfFile(buffer);
    CloseHandle(mapping);
    CloseHandle(dataReady);
    CloseHandle(bufferReady);

    std::printf("--- capture finished ---\n");
    return 0;
}
