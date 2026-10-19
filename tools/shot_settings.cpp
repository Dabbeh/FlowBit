// Renders the Settings page, with the diagnostics report expanded, at two window sizes.
//
// The diagnostic rows were unaligned: each row was a horizontal layout whose value
// label took the stretch, so readings started at different x positions in a wide
// window and names wrapped in a narrow one. The rows are now a grid with a measured
// name column.
//
// Layout is judged by looking at it, so this writes PNGs rather than only numbers.

#include "mainwindow.h"
#include "diagnosticspanel.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QPixmap>
#include <QGridLayout>
#include <QThread>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

void shoot(const char *name, const QSize &size)
{
    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(size);
    win->show();
    QApplication::processEvents();
    for (int i = 0; i < 15; ++i) { QApplication::processEvents(); QThread::msleep(10); }

    // Go to Settings, where the diagnostics panel lives.
    if (auto *side = win->findChild<Sidebar *>())
        win->onPageSelected(int(Sidebar::Settings));
    for (int i = 0; i < 25; ++i) { QApplication::processEvents(); QThread::msleep(10); }

    if (auto *panel = win->findChild<DiagnosticsPanel *>()) {
        std::printf("  diagnostics panel: %dx%d  visible=%s\n",
                    panel->width(), panel->height(), panel->isVisible() ? "yes" : "no");
    }

    const QString path = QStringLiteral("C:/Users/enaya/Desktop/Flowbit/settings-%1.png").arg(name);
    const QPixmap shot = win->grab();
    if (shot.save(path))
        std::printf("  wrote %s (%dx%d)\n", qPrintable(path), shot.width(), shot.height());
    else
        std::printf("  FAILED to write %s\n", qPrintable(path));

    win->close();
    delete win;
    QApplication::processEvents();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    std::printf("\n=== settings: maximized (1536x816) ===\n");
    shoot("max", QSize(1536, 816));

    std::printf("\n=== settings: narrow (900x700) ===\n");
    shoot("narrow", QSize(900, 700));

    return 0;
}
