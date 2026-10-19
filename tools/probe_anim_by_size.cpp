// Measures what moves when the panel opens, at SEVERAL window sizes.
//
// Reported symptoms, and why they need measuring rather than reasoning:
//
//   1. "the table headings (Application/Download/Upload/Total) move a little when
//      it opens and closes" - in a small window.
//   2. "when the window is full it still pushes the top graph to the top, and goes
//      down and so on" - i.e. the mirror of the earlier defect, but ONLY maximized.
//
// A small window and a maximized one differ in exactly one relevant way: whether
// the scroll area's canvas has LEFTOVER space (Qt: widgetResizable "takes advantage
// of extra space"). With no leftover space the layout is already at its minimum and
// nothing can redistribute; with leftover space the layout may hand it to whichever
// child will accept it, and a pinned panel plus a stretch of 1 is a tug of war.
//
// This prints, per window size and per phase (before / during / after):
//   * the canvas height, the viewport height, and their difference (leftover space)
//   * the top card's y and height, and the panel's y and height
//   * the table's viewport width and the header section positions
//
// Symptom 1 is the header positions changing; symptom 2 is the top card's y or
// height changing. Both become visible numbers here.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <windows.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QScrollArea>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace net;

namespace {

struct Row {
    qint64 ms;
    int canvasH, viewportH;
    int topY, topH;
    int panelY, panelH;
    int tableW;
    int headerTotal;
};

const char *phase(qint64 ms)
{
    if (ms < 0)
        return "before";
    if (ms <= 320)
        return "during";
    return "after ";
}

void runCase(const char *label, const QSize &size)
{
    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(size);
    auto *outer = new QVBoxLayout(win);
    outer->setContentsMargins(0, 0, 0, 0);

    // Mirror the dashboard: a scroll area whose widget is a canvas with a top row,
    // the panel, cards below, and a trailing stretch.
    auto *scroll = new QScrollArea(win);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    auto *canvas = new QWidget;
    auto *cl = new QVBoxLayout(canvas);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(12);
    scroll->setWidget(canvas);
    outer->addWidget(scroll);

    auto *top = new QWidget(canvas);
    top->setMinimumHeight(300);
    top->setObjectName(QStringLiteral("panel"));
    cl->addWidget(top);

    auto *panel = new AppActivityPanel(&monitor, canvas);
    cl->addWidget(panel);

    auto *limiter = new QWidget(canvas);
    limiter->setMinimumHeight(240);
    limiter->setObjectName(QStringLiteral("panel"));
    cl->addWidget(limiter);

    auto *sched = new QWidget(canvas);
    sched->setMinimumHeight(200);
    sched->setObjectName(QStringLiteral("panel"));
    cl->addWidget(sched);

    cl->addStretch(1);

    win->show();
    QApplication::processEvents();

    std::vector<Row> rows;
    QElapsedTimer clock;

    auto sample = [&](qint64 ms) {
        auto *table = panel->findChild<QTableWidget *>();
        Row r{};
        r.ms = ms;
        r.canvasH = canvas->height();
        r.viewportH = scroll->viewport()->height();
        r.topY = top->mapTo(canvas, QPoint(0, 0)).y();
        r.topH = top->height();
        r.panelY = panel->mapTo(canvas, QPoint(0, 0)).y();
        r.panelH = panel->height();
        if (table) {
            r.tableW = table->viewport()->width();
            int total = 0;
            for (int c = 0; c < table->columnCount(); ++c)
                total += table->columnWidth(c);
            r.headerTotal = total;
        }
        rows.push_back(r);
    };

    sample(-1);

    panel->setExpanded(true);
    clock.start();
    while (clock.elapsed() < 900) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(15);
        sample(clock.elapsed());
    }

    std::printf("\n=== %s  (window %dx%d) ===\n", label, size.width(), size.height());
    std::printf("  leftover space before: canvas %d - viewport %d = %d px\n",
                rows.front().canvasH, rows.front().viewportH,
                rows.front().canvasH - rows.front().viewportH);

    // Report the extremes observed during the reveal, which is where movement shows.
    int minTopY = rows.front().topY, maxTopY = rows.front().topY;
    int minTopH = rows.front().topH, maxTopH = rows.front().topH;
    int minTableW = rows.front().tableW, maxTableW = rows.front().tableW;
    int minHdr = rows.front().headerTotal, maxHdr = rows.front().headerTotal;
    int minCanvas = rows.front().canvasH, maxCanvas = rows.front().canvasH;

    for (const Row &r : rows) {
        minTopY = std::min(minTopY, r.topY);
        maxTopY = std::max(maxTopY, r.topY);
        minTopH = std::min(minTopH, r.topH);
        maxTopH = std::max(maxTopH, r.topH);
        if (r.tableW > 0) {
            minTableW = std::min(minTableW, r.tableW);
            maxTableW = std::max(maxTableW, r.tableW);
            minHdr = std::min(minHdr, r.headerTotal);
            maxHdr = std::max(maxHdr, r.headerTotal);
        }
        minCanvas = std::min(minCanvas, r.canvasH);
        maxCanvas = std::max(maxCanvas, r.canvasH);
    }

    std::printf("  top card y   : %d .. %d   (moved %d px)\n", minTopY, maxTopY, maxTopY - minTopY);
    std::printf("  top card h   : %d .. %d   (changed %d px)\n", minTopH, maxTopH, maxTopH - minTopH);
    std::printf("  canvas h     : %d .. %d\n", minCanvas, maxCanvas);
    std::printf("  table width  : %d .. %d   (changed %d px)\n", minTableW, maxTableW, maxTableW - minTableW);
    std::printf("  header total : %d .. %d   (changed %d px)\n", minHdr, maxHdr, maxHdr - minHdr);

    std::printf("  verdict:\n");
    std::printf("    %s  the top card does not move vertically (%d px)\n",
                (maxTopY - minTopY) <= 1 ? "PASS" : "FAIL", maxTopY - minTopY);
    std::printf("    %s  the top card keeps its height (%d px)\n",
                (maxTopH - minTopH) <= 1 ? "PASS" : "FAIL", maxTopH - minTopH);
    std::printf("    %s  the table does not change width (%d px)\n",
                (maxTableW - minTableW) <= 1 ? "PASS" : "FAIL", maxTableW - minTableW);
    std::printf("    %s  the header sections do not shift (%d px)\n",
                (maxHdr - minHdr) <= 1 ? "PASS" : "FAIL", maxHdr - minHdr);

    win->close();
    win->deleteLater();
    QApplication::processEvents();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    timeBeginPeriod(1);

    runCase("small window", QSize(900, 620));
    runCase("large window", QSize(1400, 900));
    runCase("maximized-like", QSize(1920, 1040));

    timeEndPeriod(1);
    return 0;
}
