// Verifies that theme::applyTitleBar applies the caption colour to the native title bar.
//
// THE FAULT
//
// The strip at the top of the window - title, minimise, maximise, close - is drawn by DWM, not
// Qt, and it never followed the theme: default black over the application's charcoal.
//
// HOW THIS IS VERIFIED, AND HONESTLY WHAT IS NOT
//
// Measured here:
//
//   * applyTitleBar returns true, which it does only when DwmSetWindowAttribute accepted the
//     caption colour (attribute 35, checked against S_OK inside).
//   * the raw call also succeeds, independently, from this test.
//   * it succeeds again after switching dark -> light, so the hook follows mode changes.
//   * the two themes hand DWM different colours - the light and dark backgrounds differ, so the
//     bar's colour genuinely changes with the mode rather than being pinned once.
//
// Not measurable here, stated plainly: DwmGetWindowAttribute refuses attribute 35 on this build
// (E_INVALIDARG even immediately after a successful set), so the colour Windows actually holds
// cannot be read back for comparison. The set being accepted is the strongest check available;
// the visual confirmation is the user's.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <dwmapi.h>
#include <windows.h>
#include <cstdio>

using namespace net;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

COLORREF asColorRef(const QColor &c)
{
    return RGB(c.red(), c.green(), c.blue());
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    QTemporaryDir dir;
    static SettingsStore store(dir.path() + QStringLiteral("/cfg"));
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1000, 700);
    win->show();
    pump(800);   // showEvent fires and applies the title bar theme

    HWND hwnd = reinterpret_cast<HWND>(win->winId());
    std::printf("\n== the native title bar ==\n\n");

    // ---- the attribute is supported on this build ------------------------
    COLORREF probe = 0;
    const HRESULT setHr = DwmSetWindowAttribute(hwnd, 35, &probe, sizeof(probe));
    std::printf("  DwmSetWindowAttribute(35) probe hr: 0x%08lx%s\n\n",
                static_cast<unsigned long>(setHr),
                setHr == S_OK ? " (supported)" : " (NOT supported here)");

    check(setHr == S_OK, "this build accepts the caption-colour attribute");

    // ---- dark theme ------------------------------------------------------
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();
    const bool darkApplied = theme::applyTitleBar(win);
    const COLORREF darkColour = asColorRef(theme::kBackground());

    check(darkApplied, "dark theme: the caption colour is applied through the hook");

    // And the attribute is accepted again with the exact dark colour, directly:
    const HRESULT darkSet = DwmSetWindowAttribute(hwnd, 35, &darkColour, sizeof(darkColour));
    check(darkSet == S_OK, "and DWM accepts exactly the dark theme's background colour");

    // ---- light theme -----------------------------------------------------
    theme::setMode(theme::Mode::Light);
    theme::applyApplicationTheme();
    const bool lightApplied = theme::applyTitleBar(win);
    const COLORREF lightColour = asColorRef(theme::kBackground());

    check(lightApplied, "light theme: the hook follows without a restart");
    check(lightColour != darkColour,
          QStringLiteral("and the two modes hand DWM different colours (%1 vs %2)")
              .arg(darkColour, 6, 16, QLatin1Char('0'))
              .arg(lightColour, 6, 16, QLatin1Char('0')));

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_titlebar_theme passed."
                                        : "test_titlebar_theme FAILED.");
    return g_failures == 0 ? 0 : 1;
}
