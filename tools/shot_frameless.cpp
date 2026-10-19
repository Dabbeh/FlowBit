// Renders the frameless window, so the change is judged by looking at it.
//
// The last header change looked correct in the numbers and clipped the tagline; the
// screenshot is what caught it. The same applies here, and more so: a frameless window
// either looks seamless or looks broken, and that is a visual question.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPixmap>
#include <QPushButton>
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
    pump(500);

    std::printf("\n=== %s (requested %dx%d) ===\n", name, size.width(), size.height());
    std::printf("  frameGeometry %dx%d at (%d,%d)\n",
                win->frameGeometry().width(), win->frameGeometry().height(),
                win->frameGeometry().x(), win->frameGeometry().y());
    std::printf("  geometry      %dx%d at (%d,%d)\n",
                win->geometry().width(), win->geometry().height(),
                win->geometry().x(), win->geometry().y());

    // A frameless window's frameGeometry must equal its geometry: if they differ, a
    // system frame is still being added and the band would still be there.
    const bool frameless = win->frameGeometry().size() == win->geometry().size();
    std::printf("  frameless: %s\n", frameless ? "yes" : "NO - a frame is present");

    // The title bar and its controls, measured.
    if (QWidget *bar = win->findChild<QWidget *>(QStringLiteral("titleBar"))) {
        std::printf("  title bar: %dx%d\n", bar->width(), bar->height());
        for (const QString &name : {QStringLiteral("titleBarButton"),
                                    QStringLiteral("titleBarClose")}) {
            const auto buttons = bar->findChildren<QPushButton *>(name);
            if (!buttons.isEmpty()) {
                std::printf("  %-16s %dx%d  (%d of them)\n", qPrintable(name),
                            buttons.first()->width(), buttons.first()->height(),
                            int(buttons.size()));
            }
        }
    }

    const QPixmap shot = win->grab();
    const QString path =
        QStringLiteral("C:/Users/enaya/Desktop/Flowbit/frameless-%1.png").arg(name);
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
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    shoot("small", QSize(900, 600));
    shoot("large", QSize(1500, 950));
    return 0;
}
