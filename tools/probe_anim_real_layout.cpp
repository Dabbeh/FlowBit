// Reproduces symptom 2 by using the REAL dashboard structure, not lookalike widgets.
//
// The previous probe used plain QWidgets for the cards and could not reproduce the
// "pushes the top graph" movement at any window size - the top card stayed put
// (0 px) in all three cases. So the movement is not caused by the panel's own tween
// or by the trailing stretch; it must come from a widget in the real dashboard that
// re-measures itself when the canvas changes height.
//
// The prime suspect is ResponsivePair: it decides stacked/side-by-side from the
// widget WIDTH, and applyLayout() calls setMinimumHeight(...) built from its
// children's minimumSizeHint(). A canvas height change can therefore feed back into
// the pair's own minimum height - and with three children (when the panel is
// embedded BETWEEN the two cards in stacked mode) the pair's height requirement
// grows by the panel's height, which is exactly the "graph pushed to the top" case.
//
// This probe measures the Live Speed card (the one with the graph) while the panel
// opens, using the real ResponsivePair in both orientations.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "responsivepair.h"
#include "theme.h"

#include <windows.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace net;

namespace {

struct Snap {
    qint64 ms;
    int canvasH, viewportH, scroll;
    int pairY, pairH;
    int liveY, liveH;     // the Live Speed card, i.e. the one holding the graph
    int panelY, panelH;
    int belowY;
};

void runCase(const char *label, const QSize &size, bool embedded)
{
    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(size);
    auto *outer = new QVBoxLayout(win);
    outer->setContentsMargins(0, 0, 0, 0);

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

    // The REAL pair, with two cards whose minimums resemble Live Speed and Usage.
    auto *pair = new ResponsivePair(canvas);
    auto *live = new QWidget(pair);
    live->setMinimumSize(320, 300);
    live->setObjectName(QStringLiteral("panel"));
    auto *usage = new QWidget(pair);
    usage->setMinimumSize(260, 300);
    usage->setObjectName(QStringLiteral("panel"));
    pair->setPair(live, usage, 62, 38);
    pair->setMinimumHeight(300);
    cl->addWidget(pair);

    auto *panel = new AppActivityPanel(&monitor, canvas);
    cl->addWidget(panel);

    auto *below = new QWidget(canvas);
    below->setMinimumHeight(400);
    below->setObjectName(QStringLiteral("panel"));
    cl->addWidget(below);
    cl->addStretch(1);

    win->show();
    QApplication::processEvents();

    // Mirror the dashboard's own placement rule for the stacked case.
    if (embedded)
        pair->embedMiddle(panel);
    else
        cl->insertWidget(1, panel);

    QApplication::processEvents();

    std::vector<Snap> snaps;
    QElapsedTimer clock;

    auto sample = [&](qint64 ms) {
        Snap s{};
        s.ms = ms;
        s.canvasH = canvas->height();
        s.viewportH = scroll->viewport()->height();
        s.scroll = scroll->verticalScrollBar()->value();
        s.pairY = pair->mapTo(canvas, QPoint(0, 0)).y();
        s.pairH = pair->height();
        s.liveY = live->mapTo(canvas, QPoint(0, 0)).y();
        s.liveH = live->height();
        s.panelY = panel->mapTo(canvas, QPoint(0, 0)).y();
        s.panelH = panel->height();
        s.belowY = below->mapTo(canvas, QPoint(0, 0)).y();
        snaps.push_back(s);
    };

    sample(-1);
    panel->setExpanded(true);
    clock.start();
    while (clock.elapsed() < 900) {
        QApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(15);
        sample(clock.elapsed());
    }

    const Snap &first = snaps.front();
    int minLiveY = first.liveY, maxLiveY = first.liveY;
    int minLiveH = first.liveH, maxLiveH = first.liveH;
    int minPairH = first.pairH, maxPairH = first.pairH;
    int minScroll = first.scroll, maxScroll = first.scroll;
    int minCanvas = first.canvasH, maxCanvas = first.canvasH;

    for (const Snap &s : snaps) {
        minLiveY = std::min(minLiveY, s.liveY);
        maxLiveY = std::max(maxLiveY, s.liveY);
        minLiveH = std::min(minLiveH, s.liveH);
        maxLiveH = std::max(maxLiveH, s.liveH);
        minPairH = std::min(minPairH, s.pairH);
        maxPairH = std::max(maxPairH, s.pairH);
        minScroll = std::min(minScroll, s.scroll);
        maxScroll = std::max(maxScroll, s.scroll);
        minCanvas = std::min(minCanvas, s.canvasH);
        maxCanvas = std::max(maxCanvas, s.canvasH);
    }

    std::printf("\n=== %s, %s (window %dx%d) ===\n", label,
                embedded ? "panel EMBEDDED in the pair" : "panel BELOW the pair",
                size.width(), size.height());
    std::printf("  leftover before : canvas %d - viewport %d = %d px\n",
                first.canvasH, first.viewportH, first.canvasH - first.viewportH);
    std::printf("  LIVE card y     : %d .. %d   (moved %d px)\n", minLiveY, maxLiveY, maxLiveY - minLiveY);
    std::printf("  LIVE card h     : %d .. %d   (changed %d px)\n", minLiveH, maxLiveH, maxLiveH - minLiveH);
    std::printf("  pair height     : %d .. %d   (changed %d px)\n", minPairH, maxPairH, maxPairH - minPairH);
    std::printf("  scroll value    : %d .. %d   (moved %d px)\n", minScroll, maxScroll, maxScroll - minScroll);
    std::printf("  canvas height   : %d .. %d\n", minCanvas, maxCanvas);
    std::printf("  verdict:\n");
    std::printf("    %s  the graph card keeps its height (%d px)\n",
                (maxLiveH - minLiveH) <= 1 ? "PASS" : "FAIL", maxLiveH - minLiveH);
    std::printf("    %s  the graph card keeps its canvas position (%d px)\n",
                (maxLiveY - minLiveY) <= 1 ? "PASS" : "FAIL", maxLiveY - minLiveY);
    std::printf("    %s  the viewport does not scroll during the reveal (%d px)\n",
                (maxScroll - minScroll) <= 1 ? "PASS" : "FAIL", maxScroll - minScroll);

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

    // The dashboard embeds the panel between the two cards when the pair is stacked
    // (narrow window) and places it below the pair when side by side (wide window).
    runCase("narrow", QSize(900, 620), true);
    runCase("wide small", QSize(1200, 620), false);
    runCase("wide maximized", QSize(1920, 1040), false);
    runCase("narrow maximized", QSize(1000, 1040), true);

    timeEndPeriod(1);
    return 0;
}
