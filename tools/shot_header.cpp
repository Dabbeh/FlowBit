// Renders the top of the window, so a header change can be judged by looking at it.
//
// The reported problem: the OS title bar reads "Flowbit", and immediately below it the
// sidebar repeats a mark and the word "Flowbit" with "Bandwidth Manager" on a third
// line. The name appears twice within a few dozen pixels, and the sidebar stack uses
// three rows for what the title bar already says once.
//
// Crops to the top-left region at two window sizes, so both the normal and the collapsed
// sidebar are seen.

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

void shoot(const char *name, const QSize &size)
{
    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(size);
    win->show();
    pump(400);

    // The top strip only: the header row and a little below it.
    const QPixmap full = win->grab();
    const QPixmap top = full.copy(0, 0, full.width(), 150);
    const QString path =
        QStringLiteral("C:/Users/enaya/Desktop/Flowbit/header-%1.png").arg(name);
    if (top.save(path))
        std::printf("  wrote %s (%dx%d)\n", qPrintable(path), top.width(), top.height());
    else
        std::printf("  FAILED to write %s\n", qPrintable(path));

    win->close();
    delete win;
    pump(100);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    shoot("before", QSize(1100, 800));
    return 0;
}
