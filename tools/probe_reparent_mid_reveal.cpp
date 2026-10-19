// Tests whether RE-PARENTING the panel mid-reveal is what moves the graph.
//
// Established so far: a harness with the real ResponsivePair, the real panel, all
// four window sizes, and the panel in both placements, shows the graph card moving
// 0 px and the viewport scrolling 0 px. So the reveal tween itself is not the cause
// of symptom 2 ("in a full window it pushes the top graph to the top").
//
// What the real dashboard has and the harness did not exercise is the re-placement
// path: MainWindow connects ResponsivePair::orientationChanged to placeAppPanel,
// which calls embedMiddle() (setParent + insert) or takeMiddle() (setParent(nullptr))
// on the panel. Qt REPARENTS by hiding the widget and re-showing it, so a mid-reveal
// re-placement collapses the panel to nothing for a frame, then restores it - which
// the eye reads as the content above being shoved.
//
// This probe forces exactly that: it opens the panel and re-parents it partway
// through, then reports what the surrounding widgets did.

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
    int liveY, liveH, panelH, canvasH, scroll, belowY;
    bool panelVisible;
};

void runCase(const char *label, bool reparentMidReveal)
{
    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(1400, 900);
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

    auto *pair = new ResponsivePair(canvas);
    auto *live = new QWidget(pair);
    live->setMinimumSize(300, 300);
    live->setObjectName(QStringLiteral("panel"));
    auto *usage = new QWidget(pair);
    usage->setMinimumSize(250, 300);
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

    std::vector<Snap> snaps;
    QElapsedTimer clock;

    auto sample = [&](qint64 ms) {
        Snap s{};
        s.ms = ms;
        s.liveY = live->mapTo(canvas, QPoint(0, 0)).y();
        s.liveH = live->height();
        s.panelH = panel->height();
        s.canvasH = canvas->height();
        s.scroll = scroll->verticalScrollBar()->value();
        s.belowY = below->mapTo(canvas, QPoint(0, 0)).y();
        s.panelVisible = panel->isVisible();
        snaps.push_back(s);
    };

    sample(-1);
    panel->setExpanded(true);
    clock.start();

    bool reparented = false;
    while (clock.elapsed() < 800) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);

        // Partway through the reveal, do what orientationChanged would do.
        if (reparentMidReveal && !reparented && clock.elapsed() > 120) {
            reparented = true;
            pair->embedMiddle(panel);
        }
        QThread::msleep(12);
        sample(clock.elapsed());
    }

    int minLiveY = snaps.front().liveY, maxLiveY = snaps.front().liveY;
    int minLiveH = snaps.front().liveH, maxLiveH = snaps.front().liveH;
    int minBelow = snaps.front().belowY, maxBelow = snaps.front().belowY;
    int minCanvas = snaps.front().canvasH, maxCanvas = snaps.front().canvasH;
    int hiddenFrames = 0;
    int minPanelH = snaps.front().panelH, maxPanelH = snaps.front().panelH;

    for (const Snap &s : snaps) {
        minLiveY = std::min(minLiveY, s.liveY);
        maxLiveY = std::max(maxLiveY, s.liveY);
        minLiveH = std::min(minLiveH, s.liveH);
        maxLiveH = std::max(maxLiveH, s.liveH);
        minBelow = std::min(minBelow, s.belowY);
        maxBelow = std::max(maxBelow, s.belowY);
        minCanvas = std::min(minCanvas, s.canvasH);
        maxCanvas = std::max(maxCanvas, s.canvasH);
        minPanelH = std::min(minPanelH, s.panelH);
        maxPanelH = std::max(maxPanelH, s.panelH);
        if (!s.panelVisible)
            ++hiddenFrames;
    }

    std::printf("\n=== %s ===\n", label);
    std::printf("  graph card y    : %d .. %d  (moved %d px)\n", minLiveY, maxLiveY, maxLiveY - minLiveY);
    std::printf("  graph card h    : %d .. %d  (changed %d px)\n", minLiveH, maxLiveH, maxLiveH - minLiveH);
    std::printf("  panel height    : %d .. %d\n", minPanelH, maxPanelH);
    std::printf("  card below y    : %d .. %d  (moved %d px)\n", minBelow, maxBelow, maxBelow - minBelow);
    std::printf("  canvas height   : %d .. %d\n", minCanvas, maxCanvas);
    std::printf("  panel hidden in %d sample(s)\n", hiddenFrames);
    std::printf("  verdict: %s\n",
                (maxLiveH - minLiveH) <= 1 && (maxLiveY - minLiveY) <= 1
                    ? "the graph card is undisturbed"
                    : "THE GRAPH CARD MOVES");

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

    runCase("reveal, panel NOT re-parented", false);
    runCase("reveal, panel RE-PARENTED mid-tween (what orientationChanged does)", true);

    timeEndPeriod(1);
    return 0;
}
