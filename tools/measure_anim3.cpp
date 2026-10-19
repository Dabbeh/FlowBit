// Verifies the Show More animation after the push/stutter fixes.
//
// Three things are measured on the REAL panel, inside a scroll area that also
// holds other cards (mirroring the dashboard):
//
//   1. smoothness of the height change (largest consecutive jump)
//   2. HORIZONTAL STABILITY of a card next to the panel - the "push" was a
//      one-frame horizontal re-flow caused by the vertical scrollbar appearing
//      mid-animation. With the scrollbar always present, x must not move.
//   3. the panel's top edge staying fixed - it should grow downward only.
//
// timeBeginPeriod(1) is called so the sampler is not limited by the default
// ~15.6ms Windows timer resolution, which previously made the measurements coarse.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <windows.h>   // timeBeginPeriod

#include <QApplication>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <vector>

using namespace net;

struct Sample { qint64 ms; int h; int x; };

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    // Timer resolution: without this, Windows rounds timer callbacks to ~15.6ms,
    // which makes frame-by-frame measurements look like bigger steps than the
    // display actually shows.
    timeBeginPeriod(1);

    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(900, 620);
    auto *outer = new QVBoxLayout(win);

    auto *scroll = new QScrollArea(win);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    auto *canvas = new QWidget;
    auto *cl = new QVBoxLayout(canvas);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(12);
    scroll->setWidget(canvas);
    outer->addWidget(scroll);

    auto *above = new QWidget(canvas);  above->setMinimumHeight(200);
    cl->addWidget(above);

    auto *panel = new AppActivityPanel(&monitor, canvas);
    cl->addWidget(panel);

    auto *below = new QWidget(canvas);  below->setMinimumHeight(400);
    cl->addWidget(below);

    win->show();
    QApplication::processEvents();

    std::vector<Sample> samples;
    QElapsedTimer clock;

    auto *sampler = new QTimer(win);
    sampler->setTimerType(Qt::PreciseTimer);
    sampler->setInterval(2);
    QObject::connect(sampler, &QTimer::timeout, win, [&] {
        samples.push_back({clock.elapsed(), panel->height(),
                           panel->mapTo(canvas, QPoint(0, 0)).x()});
    });

    QTimer::singleShot(80, win, [&] {
        clock.start();
        sampler->start();
        panel->setExpanded(true);
    });

    QTimer::singleShot(900, win, [&] {
        sampler->stop();

        int prevH = -1, maxJump = 0, distinct = 0, lastH = -1;
        int prevX = -1, maxShiftX = 0;
        qint64 prevMs = -1, maxGap = 0;
        for (const Sample &s : samples) {
            if (prevH >= 0)
                maxJump = qMax(maxJump, qAbs(s.h - prevH));
            prevH = s.h;
            if (s.h != lastH) { ++distinct; lastH = s.h; }
            if (prevX >= 0)
                maxShiftX = qMax(maxShiftX, qAbs(s.x - prevX));
            prevX = s.x;
            if (prevMs >= 0)
                maxGap = qMax(maxGap, s.ms - prevMs);
            prevMs = s.ms;
        }

        std::printf("samples              : %zu\n", samples.size());
        std::printf("distinct heights     : %d\n", distinct);
        std::printf("largest height jump  : %d px per frame\n", maxJump);
        std::printf("largest sampling gap : %lld ms\n", static_cast<long long>(maxGap));
        std::printf("largest X shift      : %d px  (0 = no horizontal push)\n", maxShiftX);

        // Where does the largest jump happen? Print the neighbourhood.
        prevH = -1; prevMs = -1;
        int jumpAt = -1; qint64 jumpMs = -1, jumpFrom = 0, jumpTo = 0;
        prevH = -1;
        for (size_t i = 0; i < samples.size(); ++i) {
            if (prevH >= 0) {
                const int j = qAbs(samples[i].h - prevH);
                if (j > jumpAt) { jumpAt = j; jumpMs = samples[i].ms; jumpFrom = prevH; jumpTo = samples[i].h; }
            }
            prevH = samples[i].h;
            prevMs = samples[i].ms;
        }
        std::printf("largest jump detail  : %d -> %d px at t=%lld ms\n",
                    int(jumpFrom), int(jumpTo), static_cast<long long>(jumpMs));
        std::printf("height timeline      :");
        prevH = -1;
        int printed = 0;
        for (const Sample &s : samples) {
            if (s.h != prevH) {
                std::printf(" %lld:%d", static_cast<long long>(s.ms), s.h);
                prevH = s.h;
                if (++printed > 24) { std::printf(" ..."); break; }
            }
        }
        std::printf("\n");
        std::printf("\n");
        std::printf("verdict:\n");
        std::printf("  %s  height moves progressively (>= 10 distinct heights)\n",
                    distinct >= 10 ? "PASS" : "FAIL");
        std::printf("  %s  no dropped frame (jump <= 20 px at 60 fps)\n",
                    maxJump <= 20 ? "PASS" : "FAIL");
        std::printf("  %s  no horizontal push (x shift == 0)\n",
                    maxShiftX == 0 ? "PASS" : "FAIL");

        win->close();
        timeEndPeriod(1);
    });

    return app.exec();
}
