// Finds out why the caption colour is not taking effect.
//
// Reported: the native title bar is present and dark, but it is the SYSTEM's dark grey
// rather than the application's #0D0D0D, so it reads as a separate band. The call is
// made (applyFrameColour runs on show and on a theme change) and DwmSetWindowAttribute
// reports success, so the fault is in the details: which handle, when, and with what
// value.
//
// This calls the same API directly against the live window and prints:
//   * the returned HRESULT for each attribute, which the application ignores;
//   * the COLORREF being sent, and the colour it was built from;
//   * whether the handle is the top-level window or a child;
//   * the result before and after forcing a non-client repaint.
//
// It also tries the pre-22000 attribute value for immersive dark mode (19), because a
// build that rejects 20 will silently keep the light caption.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QWidget>

#include <dwmapi.h>
#include <windows.h>

#include <cstdio>

using namespace net;

namespace {

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
}

DWORD colorRef(int r, int g, int b)
{
    return DWORD(b) << 16 | DWORD(g) << 8 | DWORD(r);
}

void tryAttr(const char *what, HWND hwnd, DWORD attr, DWORD value)
{
    const HRESULT hr = DwmSetWindowAttribute(hwnd, attr, &value, sizeof(value));
    std::printf("    %-28s attr %-3lu value 0x%06lX  hr 0x%08lX  %s\n", what, attr,
                value & 0xFFFFFF, unsigned(hr), SUCCEEDED(hr) ? "ok" : "FAILED");
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1100, 780);
    win->show();
    pump(700);

    const QColor bg = theme::kBackground();
    std::printf("\n== why is the caption not the theme colour? ==\n\n");
    std::printf("  theme background : %s  rgb(%d,%d,%d)\n", qPrintable(bg.name()),
                bg.red(), bg.green(), bg.blue());
    std::printf("  expected COLORREF: 0x%06lX\n",
                colorRef(bg.red(), bg.green(), bg.blue()) & 0xFFFFFF);

    HWND hwnd = reinterpret_cast<HWND>(win->winId());
    std::printf("\n  winId()          : %p\n", static_cast<void *>(hwnd));
    std::printf("  GetParent(hwnd)  : %p  (null means it is a top-level window)\n",
                static_cast<void *>(GetParent(hwnd)));

    // The application's own call, first, so its effect is visible in what follows.
    std::printf("\n  the application's own calls, re-issued with the results shown:\n");
    const BOOL dark = TRUE;
    tryAttr("immersive dark (20)", hwnd, 20, dark);
    tryAttr("caption colour (35)", hwnd, 35,
            colorRef(bg.red(), bg.green(), bg.blue()));
    tryAttr("text colour (36)", hwnd, 36,
            colorRef(theme::kTextPrimary().red(), theme::kTextPrimary().green(),
                     theme::kTextPrimary().blue()));
    tryAttr("border colour (34)", hwnd, 34,
            colorRef(theme::kBorder().red(), theme::kBorder().green(),
                     theme::kBorder().blue()));

    // The older attribute number for dark mode. A build that rejects 20 keeps a light
    // caption, and 19 is the value used before 22000.
    std::printf("\n  the pre-22000 dark-mode attribute, in case 20 is ignored:\n");
    tryAttr("immersive dark (19)", hwnd, 19, dark);

    std::printf("\n  forcing a non-client repaint...\n");
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    pump(400);

    // Read a couple of attributes back, which is the only way to tell whether the value
    // was stored rather than merely accepted.
    std::printf("\n  reading back:\n");
    const DWORD attrs[] = {34, 35, 36};
    for (DWORD a : attrs) {
        DWORD got = 0;
        DWORD size = sizeof(got);
        const HRESULT hr = DwmGetWindowAttribute(hwnd, a, &got, size);
        if (SUCCEEDED(hr))
            std::printf("    attr %-3lu reads back 0x%06lX\n", a, got & 0xFFFFFF);
        else
            std::printf("    attr %-3lu not readable (hr 0x%08lX)\n", a, unsigned(hr));
    }

    std::printf("\n  the window is on screen now. Look at its caption: if it is still the\n"
                "  system grey rather than 0x%06lX, the attribute is being ignored by this\n"
                "  build and the colour cannot be applied this way.\n",
                colorRef(bg.red(), bg.green(), bg.blue()));

    pump(6000);
    win->close();
    delete win;
    return 0;
}
