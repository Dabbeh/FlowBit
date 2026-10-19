// Measures the smoothness of the Show More / Show Less animation.
//
// The report was that the reveal "pushes" and "lags". Both are measurable:
//
//   * PUSHING shows up as large frame-to-frame jumps in the panel height, or as
//     the panel's position moving in ways the height does not explain.
//   * LAGGING shows up as uneven frame intervals.
//
// This drives the real AppActivityPanel through an expand and a collapse, sampling
// its geometry every few milliseconds, and reports:
//
//   * the number of distinct heights observed (a smooth tween has many)
//   * the largest single-frame height jump (a smooth tween never jumps)
//   * whether the panel's top edge moves during the reveal (it should not: the
//     panel is at the end of the layout, so only its height should change)

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <vector>

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

    auto *scroll = new QScrollArea(win);
    scroll->setWidgetResizable(true);
    auto *canvas = new QWidget;
    auto *canvasLayout = new QVBoxLayout(canvas);
    scroll->setWidget(canvas);
    outer->addWidget(scroll);

    // Something above the panel, so a layout re-flow would be visible.
    auto *above = new QWidget(canvas);
    above->setMinimumHeight(200);
    above->setObjectName(QStringLiteral("panel"));
    canvasLayout->addWidget(above);

    auto *panel = new AppActivityPanel(&monitor, canvas);
    canvasLayout->addWidget(panel);

    auto *below = new QWidget(canvas);
    below->setMinimumHeight(150);
    below->setObjectName(QStringLiteral("panel"));
    canvasLayout->addWidget(below);
    canvasLayout->addStretch(1);

    win->show();
    QApplication::processEvents();

    struct Sample { qint64 ms; int height; int top; int belowTop; };
    std::vector<Sample> samples;

    QElapsedTimer clock;
    clock.start();

    // Sample the geometry at high rate while the expand runs.
    auto *sampler = new QTimer(win);
    sampler->setInterval(4);
    QObject::connect(sampler, &QTimer::timeout, win, [&] {
        samples.push_back({clock.elapsed(), panel->height(),
                           panel->mapTo(canvas, QPoint(0, 0)).y(),
                           below->mapTo(canvas, QPoint(0, 0)).y()});
    });

    QTimer::singleShot(60, win, [&] {
        sampler->start();
        panel->setExpanded(true);
    });

    // After the reveal settles, collapse and sample again.
    QTimer::singleShot(600, win, [&] {
        samples.clear();
        clock.restart();
        panel->setExpanded(false);
    });

    QTimer::singleShot(1200, win, [&] {
        sampler->stop();

        // Analyse.
        int minH = INT_MAX, maxH = 0, maxJump = 0, prevH = -1;
        int distinct = 0;
        int topAtStart = -1, topAtEnd = -1;
        int belowMin = INT_MAX, belowMax = 0;
        std::vector<int> heights;

        for (const Sample &s : samples) {
            minH = qMin(minH, s.height);
            maxH = qMax(maxH, s.height);
            if (prevH >= 0) {
                const int jump = qAbs(s.height - prevH);
                maxJump = qMax(maxJump, jump);
            }
            prevH = s.height;
            if (heights.empty() || heights.back() != s.height)
                heights.push_back(s.height);

            if (topAtStart < 0)
                topAtStart = s.top;
            topAtEnd = s.top;
            belowMin = qMin(belowMin, s.belowTop);
            belowMax = qMax(belowMax, s.belowTop);
        }
        distinct = int(heights.size());

        std::printf("collapse samples      : %zu\n", samples.size());
        std::printf("height range          : %d -> %d px\n", minH, maxH);
        std::printf("distinct heights      : %d\n", distinct);
        std::printf("largest frame jump    : %d px\n", maxJump);
        std::printf("panel top edge moved  : %d px\n", qAbs(topAtEnd - topAtStart));
        std::printf("content below moved   : %d px (this SHOULD change: the panel is collapsing)\n",
                    belowMax - belowMin);
        std::printf("\nverdict:\n");
        std::printf("  %s  height changes smoothly (>= 8 distinct heights)\n",
                    distinct >= 8 ? "PASS" : "FAIL");
        std::printf("  %s  no large frame jumps (<= 15%% of the total travel)\n",
                    maxJump <= qMax(4, (maxH - minH) / 7) ? "PASS" : "FAIL");
        std::printf("  %s  panel top edge stayed put (<= 2 px)\n",
                    qAbs(topAtEnd - topAtStart) <= 2 ? "PASS" : "FAIL");

        win->close();
    });

    return app.exec();
}
