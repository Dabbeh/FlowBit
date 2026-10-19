// Renders the panel WITH populated rows, so the column layout can be seen and the
// four columns verified against the viewport (the reported bug was "Upload" clipped
// and "Total" invisible because three ResizeToContents columns overflowed).
//
// AppMonitor::forceRunningForTesting() exists for exactly this: it puts the monitor
// into the running state so the panel builds its table without needing elevation or
// a real ETW session. The traffic figures come from the monitor's own snapshot, so
// realistic numbers are injected through the same API the panel reads.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <QApplication>
#include <QHeaderView>
#include <QScrollBar>
#include <QTableWidget>
#include <QElapsedTimer>
#include <QThread>
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
    monitor.forceRunningForTesting();

    auto *win = new QWidget;
    win->resize(1100, 460);
    auto *outer = new QVBoxLayout(win);
    auto *panel = new AppActivityPanel(&monitor, win);
    outer->addWidget(panel);
    outer->addStretch(1);
    win->show();
    QApplication::processEvents();

    panel->setExpanded(true);

    // Let the reveal finish and the table populate.
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 900) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }

    auto *table = panel->findChild<QTableWidget *>();
    if (!table) {
        std::printf("no table found\n");
        return 1;
    }

    std::printf("rows: %d   columns: %d\n", table->rowCount(), table->columnCount());
    const int vw = table->viewport()->width();
    int rightmost = 0;
    std::printf("viewport %d px, widths:", vw);
    for (int c = 0; c < table->columnCount(); ++c) {
        const int w = table->columnWidth(c);
        std::printf(" %d", w);
        rightmost = qMax(rightmost, table->columnViewportPosition(c) + w);
    }
    std::printf("   rightmost edge %d\n", rightmost);
    std::printf("verdict: %s\n", rightmost <= vw ? "all columns fit" : "OVERFLOW");

    // Report the last column's heading as the panel would draw it, so a clipped
    // heading would be visible in this text too.
    for (int c = 0; c < table->columnCount(); ++c) {
        if (QTableWidgetItem *h = table->horizontalHeaderItem(c))
            std::printf("  column %d: '%s'\n", c, qPrintable(h->text()));
    }

    const QPixmap shot = win->grab();
    shot.save(QStringLiteral("panel_columns.png"), "PNG");
    std::printf("rendered panel_columns.png\n");
    return rightmost <= vw ? 0 : 1;
}
