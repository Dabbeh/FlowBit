// Checks whether the native title bar can be recoloured on this Windows build, before
// replacing the custom bar with it.
//
// The idea: keep the real system frame, which brings the genuine minimise/maximise/close
// buttons, snapping, drag-to-top, the system menu and edge resizing - all the things the
// hand-built title bar had to reimplement - and recolour it with DwmSetWindowAttribute so
// it matches the application instead of looking like a separate band.
//
// Attributes used (Windows 11, build 22000+):
//     20  DWMWA_USE_IMMERSIVE_DARK_MODE   - dark caption
//     34  DWMWA_BORDER_COLOR              - frame border
//     35  DWMWA_CAPTION_COLOR             - the caption background
//     36  DWMWA_TEXT_COLOR                - the caption text and glyph colour
//
// Implemented with GetProcAddress rather than a link-time import so the program still
// runs on a build without them; a missing function is reported rather than crashing.
//
// This opens two windows side by side: the default frame and the recoloured one, so the
// difference is visible rather than assumed.

#include <windows.h>
#include <dwmapi.h>

#include <cstdio>

namespace {

using DwmSetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);

DwmSetWindowAttributeFn resolve()
{
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm)
        return nullptr;
    return reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwm, "DwmSetWindowAttribute"));
}

bool setAttr(DwmSetWindowAttributeFn fn, HWND hwnd, DWORD attr, DWORD value)
{
    const HRESULT hr = fn(hwnd, attr, &value, sizeof(value));
    std::printf("    attribute %-3lu = 0x%08lX  %s\n", attr, value,
                SUCCEEDED(hr) ? "accepted" : "REFUSED");
    return SUCCEEDED(hr);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    std::printf("=== can the native title bar be recoloured here? ===\n\n");

    auto fn = resolve();
    if (!fn) {
        std::printf("  DwmSetWindowAttribute not available - the native bar cannot be\n"
                    "  recoloured on this system, so the custom bar is the only option.\n");
        return 1;
    }
    std::printf("  DwmSetWindowAttribute resolved from dwmapi.dll\n\n");

    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FlowbitDwmProbe";
    wc.hbrBackground = CreateSolidBrush(RGB(0x0D, 0x0D, 0x0D));
    RegisterClassW(&wc);

    // The application's own background, taken from the theme: #0D0D0D.
    const DWORD background = 0x000D0D0D;   // COLORREF is 0x00BBGGRR
    const DWORD text = 0x00F3F5F7;         // the theme's primary text
    const DWORD border = 0x002F2F2F;

    std::printf("  window 1: default frame\n");
    HWND plain = CreateWindowExW(0, wc.lpszClassName, L"Flowbit - default title bar",
                                 WS_OVERLAPPEDWINDOW, 60, 60, 700, 220, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    if (plain)
        ShowWindow(plain, SW_SHOW);

    std::printf("  window 2: recoloured frame\n");
    HWND themed = CreateWindowExW(0, wc.lpszClassName, L"Flowbit - recoloured title bar",
                                  WS_OVERLAPPEDWINDOW, 120, 320, 700, 220, nullptr,
                                  nullptr, wc.hInstance, nullptr);
    if (themed) {
        // 20 before 22000, but this build takes the modern attributes directly.
        setAttr(fn, themed, 20, TRUE);        // immersive dark mode
        setAttr(fn, themed, 34, border);      // border colour
        setAttr(fn, themed, 35, background);  // caption colour
        setAttr(fn, themed, 36, text);        // caption text colour
        ShowWindow(themed, SW_SHOW);
        // Force a non-client repaint, or the caption keeps its old colour until something
        // else invalidates it.
        SetWindowPos(themed, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    std::printf("\n  both windows are open. The lower one should have a caption matching\n"
                "  the application's background (#0D0D0D) rather than the system colour,\n"
                "  with the ordinary Windows buttons.\n");
    std::printf("\n  closing in 12 seconds...\n");
    Sleep(12000);
    if (plain)
        DestroyWindow(plain);
    if (themed)
        DestroyWindow(themed);
    return 0;
}
