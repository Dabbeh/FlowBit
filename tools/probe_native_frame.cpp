// Verifies the native caption is actually recoloured, and that it follows a theme change.
//
// The window keeps the real Windows frame - so the minimise, maximise and close buttons
// are the genuine ones, and snapping, dragging, the system menu and edge resizing are all
// handled by Windows - but its caption, text and border colours are set to the
// application's, so the band matches the window instead of sitting on it in the system's
// own colour.
//
// What can be checked from inside the process is that the calls are made and accepted.
// Whether the pixels look right is a matter for a screenshot, which this also writes.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPixmap>
#include <QThread>
#include <QWidget>

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

// Asks Windows what colour it currently thinks the caption is, which is the honest check:
// the call being accepted is not the same as the value having taken effect.
void reportCaptionColour(const char *when)
{
    std::printf("  %-22s ", when);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using Fn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        auto get = reinterpret_cast<Fn>(GetProcAddress(dwm, "DwmGetWindowAttribute"));
        if (!get) {
            std::printf("DwmGetWindowAttribute unavailable\n");
            return;
        }
        // There is no getter for the caption colour itself, so the border colour (34) is
        // read back as evidence the attributes are being applied at all.
        DWORD border = 0;
        const HRESULT hr = get(GetActiveWindow(), 34, &border, sizeof(border));
        if (SUCCEEDED(hr)) {
            std::printf("border colour reads back 0x%06lX\n", border & 0xFFFFFF);
        } else {
            std::printf("border colour not readable (hr 0x%08lX)\n", unsigned(hr));
        }
    }
}

void run(const char *label, const QSize &size, bool light)
{
    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    theme::setMode(light ? theme::Mode::Light : theme::Mode::Dark);
    theme::applyApplicationTheme();

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(size);
    win->show();
    pump(500);

    std::printf("\n=== %s (%s theme) ===\n", label, light ? "light" : "dark");

    // A system frame must be present: frameGeometry is larger than geometry when there is
    // one, which is the opposite of the frameless case this replaced.
    const QSize frame = win->frameGeometry().size();
    const QSize client = win->geometry().size();
    std::printf("  frameGeometry %dx%d, geometry %dx%d\n",
                frame.width(), frame.height(), client.width(), client.height());
    std::printf("  native frame present: %s\n",
                frame.height() > client.height() ? "yes" : "NO - still frameless");
    reportCaptionColour("caption colour:");

    // The theme's own values, so the screenshot can be compared against them.
    std::printf("  theme background  : %s\n", qPrintable(theme::kBackground().name()));
    std::printf("  theme text        : %s\n", qPrintable(theme::kTextPrimary().name()));
    std::printf("  theme border      : %s\n", qPrintable(theme::kBorder().name()));

    const QPixmap shot = win->grab();
    const QString path = QStringLiteral("C:/Users/enaya/Desktop/Flowbit/native-%1.png")
                             .arg(light ? "light" : "dark");
    if (shot.save(path))
        std::printf("  wrote %s (%dx%d)\n", qPrintable(path), shot.width(), shot.height());

    win->close();
    delete win;
    pump(100);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    run("dark", QSize(1100, 780), false);
    run("light", QSize(1100, 780), true);
    return 0;
}
