// Reports the panel's measured heights in each state, to explain why the reveal
// animates to one height and then snaps to another.
//
// Measured symptom: the tween travels to ~512px, then a single correction shrinks
// the panel to ~188px - a 324px jump at the end of the animation. The reveal target
// and the settled content height disagree, and the correction (which is correct in
// itself) exposes that as a visible step.
//
// This prints the three numbers that must agree for the motion to be seamless:
//   * sizeHint() before any rows exist   (the reveal target is measured here)
//   * sizeHint() with the table hidden   (the empty state)
//   * sizeHint() with rows present       (what the panel settles to)

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

using namespace net;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(900, 700);
    auto *outer = new QVBoxLayout(win);
    auto *panel = new AppActivityPanel(&monitor, win);
    outer->addWidget(panel);
    outer->addStretch(1);
    win->show();
    QApplication::processEvents();

    auto *table = panel->findChild<QTableWidget *>();

    std::printf("=== panel height accounting ===\n");
    std::printf("  collapsed, before any expand\n");
    std::printf("    panel sizeHint          : %d\n", panel->sizeHint().height());
    std::printf("    panel minimumSizeHint   : %d\n", panel->minimumSizeHint().height());

    if (table) {
        std::printf("    table minimumHeight     : %d\n", table->minimumHeight());
        std::printf("    table sizeHint          : %d\n", table->sizeHint().height());
        std::printf("    table isVisible         : %s\n", table->isVisible() ? "yes" : "no");
    }

    // Now expand and let it settle.
    panel->setExpanded(true);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 800) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }

    std::printf("\n  after the reveal settles\n");
    std::printf("    panel height            : %d\n", panel->height());
    std::printf("    panel sizeHint          : %d\n", panel->sizeHint().height());
    std::printf("    panel minimumSizeHint   : %d\n", panel->minimumSizeHint().height());
    if (table) {
        std::printf("    table height            : %d\n", table->height());
        std::printf("    table sizeHint          : %d\n", table->sizeHint().height());
        std::printf("    table rows              : %d\n", table->rowCount());
        std::printf("    table visible           : %s\n", table->isVisible() ? "yes" : "no");
    }

    // Children breakdown, to see which one is claiming the height.
    std::printf("\n  direct children (height / sizeHint / minimum):\n");
    for (QObject *child : panel->children()) {
        auto *w = qobject_cast<QWidget *>(child);
        if (!w || !w->isVisible())
            continue;
        std::printf("    %-28s %4d / %4d / %4d\n",
                    qPrintable(w->objectName().isEmpty()
                                   ? QString::fromLatin1(w->metaObject()->className())
                                   : w->objectName()),
                    w->height(), w->sizeHint().height(), w->minimumSizeHint().height());
    }

    return 0;
}
