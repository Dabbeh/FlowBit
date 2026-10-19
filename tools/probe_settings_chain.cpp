// Reports the Settings page's own scroll area, found by walking from the page rather than
// by asking the window for "a scroll area".
//
// The window contains five QScrollArea objects - the dashboard body, the scheduler list, the
// statistics page, the settings page, and the rule list inside a card. findChild<QScrollArea*>()
// returns whichever was constructed first, which is the dashboard's. An earlier version of
// this probe did that and reported a 0px viewport, which looked like a fault in the settings
// page and was in fact a measurement of a different page entirely.
//
// This walks down from the current page instead, so it can only find that page's own area.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
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
        QThread::msleep(5);
    }
}

// The first scroll area that is a descendant of `root`, and nothing above it.
QScrollArea *ownScrollArea(QWidget *root)
{
    for (QScrollArea *a : root->findChildren<QScrollArea *>()) {
        for (QWidget *p = a->parentWidget(); p; p = p->parentWidget()) {
            if (p == root)
                return a;
        }
    }
    return nullptr;
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
    win->resize(1280, 820);
    win->show();
    pump(700);

    // Switch pages the way the application does, not by emitting the sidebar's signal.
    win->onPageSelected(int(Sidebar::Settings));
    pump(700);

    std::printf("\n== the settings page, measured through its own widgets ==\n");

    auto *stack = win->findChild<QStackedWidget *>();
    if (!stack) {
        std::printf("  no stack found\n");
        return 1;
    }
    std::printf("  the stack shows index %d of %d\n", stack->currentIndex(), stack->count());

    QWidget *page = stack->currentWidget();
    std::printf("  the current page is %dx%d visible=%d\n", page->width(), page->height(),
                page->isVisible() ? 1 : 0);

    QWidget *card = nullptr;
    for (QLabel *l : win->findChildren<QLabel *>()) {
        if (l->text() == QLatin1String("Launch Flowbit when Windows starts")) {
            QWidget *p = l->parentWidget();
            while (p && !qobject_cast<QFrame *>(p))
                p = p->parentWidget();
            card = p;
            break;
        }
    }
    if (!card) {
        std::printf("  the Startup card was not found on the current page\n");
        return 1;
    }
    std::printf("  the Startup card is %dx%d\n", card->width(), card->height());

    QScrollArea *area = ownScrollArea(page);
    if (!area) {
        std::printf("  the current page has no scroll area of its own\n");
    } else {
        std::printf("  its own scroll area: %dx%d visible=%d\n", area->width(), area->height(),
                    area->isVisible() ? 1 : 0);
        std::printf("    viewport : %dx%d\n", area->viewport()->width(),
                    area->viewport()->height());
        std::printf("    content  : %dx%d\n", area->widget() ? area->widget()->width() : -1,
                    area->widget() ? area->widget()->height() : -1);
        QScrollBar *bar = area->verticalScrollBar();
        std::printf("    scrollbar: value=%d max=%d\n", bar->value(), bar->maximum());

        if (area->widget()) {
            int y = 0;
            for (QWidget *w = card; w && w != area->widget(); w = w->parentWidget())
                y += w->y();
            const int reach = bar->maximum() + area->viewport()->height();
            std::printf("    the card starts %dpx down; scrolling reaches %dpx\n", y, reach);
            std::printf("    reachable: %s\n", (reach >= y + card->height()) ? "yes" : "NO");
        }
    }

    win->close();
    delete win;
    return 0;
}
