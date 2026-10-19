// Prints the fonts and metrics the title bar's sizing is derived from, so the fix can be
// based on the real numbers rather than on arithmetic done in the head.
//
// The sizing returned a constant 34 at every window size. Reading the code explains it:
// qMax(fontHeight + 14, 34) where fontHeight + 14 is about 31 - so the 34 floor always
// wins and the font is never consulted. Before changing it, the actual metrics are worth
// knowing: the interface font, the metrics the title bar sees, and the same at the
// widget's own font, which can differ from the application's.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "titlebar.h"
#include "theme.h"

#include <windows.h>

#include <QApplication>
#include <QFontMetrics>
#include <QPushButton>
#include <QWidget>

#include <cstdio>

using namespace net;

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
    win->resize(1200, 800);
    win->show();
    for (int i = 0; i < 30; ++i)
        QApplication::processEvents();

    const QFont appFont = QApplication::font();
    const QFontMetrics appFm(appFont);
    std::printf("\n== fonts and metrics ==\n");
    std::printf("  application font    : %s  %gpt  pixelSize %d\n",
                qPrintable(appFont.family()), appFont.pointSizeF(), appFont.pixelSize());
    std::printf("  application metrics : height %d  ascent %d  descent %d\n",
                appFm.height(), appFm.ascent(), appFm.descent());
    std::printf("  fontHeight + 14     : %d   (the value 34 was flooring)\n",
                appFm.height() + 14);
    std::printf("  devicePixelRatio    : %g\n", win->devicePixelRatioF());

    if (auto *bar = win->findChild<TitleBar *>()) {
        const QFont barFont = bar->font();
        const QFontMetrics barFm(barFont);
        std::printf("\n  title bar font      : %s  %gpt\n", qPrintable(barFont.family()),
                    barFont.pointSizeF());
        std::printf("  title bar metrics   : height %d\n", barFm.height());
        std::printf("  title bar size      : %dx%d\n", bar->width(), bar->height());
        std::printf("  TitleBar::barHeight(): %d\n", TitleBar::barHeight());

        for (const QString &name : {QStringLiteral("titleBarButton"),
                                    QStringLiteral("titleBarClose")}) {
            const auto buttons = bar->findChildren<QPushButton *>(name);
            for (QPushButton *b : buttons) {
                std::printf("  %-16s : %dx%d  font %gpt  text \"%s\"\n", qPrintable(name),
                            b->width(), b->height(), b->font().pointSizeF(),
                            qPrintable(b->text()));
            }
        }
    }

    // What the OS frame would have used, for comparison: the metrics of a window's own
    // caption are available through the system, and the native title bar is the size the
    // user is used to.
    std::printf("\n  for reference, the native caption height on this display:\n");
    std::printf("    GetSystemMetrics(SM_CYCAPTION) = %d\n", GetSystemMetrics(SM_CYCAPTION));
    std::printf("    GetSystemMetrics(SM_CXSMSIZE)  = %d\n", GetSystemMetrics(SM_CXSMSIZE));

    win->close();
    delete win;
    return 0;
}
