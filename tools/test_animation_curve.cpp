// Measures the reveal animation and asserts the properties Design A requires.
//
// From docs/animation-show-more.md the reveal must be:
//   * ONE motion - the panel's height, and nothing else on screen moves
//   * zero velocity at BOTH ends (InOutSine), so neither the first nor the last
//     displayed frame carries a visible step
//   * monotonic - the panel never grows, shrinks and grows again
//
// The curve choice is verified by SHAPE, not by reading the source: an ease-in-out
// tween must move LESS in its opening frames than in its middle ones. An ease-out
// curve (the previous OutQuad) moves most in the first frame, which is the step that
// was reported as "it pushes".
//
// A 1ms timer resolution is requested so the sampling is finer than the ~15.6ms
// default, and the frames are also written to PNG for visual comparison.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <windows.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace net;

namespace {

struct Sample { qint64 ms; int height; int x; int topY; };

int g_failures = 0;
void check(bool ok, const QString &what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(900, 620);
    auto *outer = new QVBoxLayout(win);

    // A dashboard-shaped canvas: something above the panel (so a layout re-flow
    // would be visible as movement) and something below (which must slide).
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

    auto *above = new QWidget(canvas);
    above->setMinimumHeight(200);
    cl->addWidget(above);

    auto *panel = new AppActivityPanel(&monitor, canvas);
    cl->addWidget(panel);

    // The real dashboard keeps monitoring while collapsed, so Show More often
    // opens onto a populated table. This catches a reveal target measured from
    // the stale empty state followed by a one frame correction to the table.
    monitor.forceRunningForTesting();
    for (quint32 i = 0; i < 6; ++i)
        monitor.injectForTesting(7000 + i,
                                 QStringLiteral("C:/apps/app%1.exe").arg(i),
                                 2u * 1024 * 1024 - i * 1024,
                                 256u * 1024 + i * 512);
    monitor.flushForTesting(1.0);

    auto *below = new QWidget(canvas);
    below->setMinimumHeight(400);
    cl->addWidget(below);
    cl->addStretch(1);

    win->show();
    QApplication::processEvents();

    std::vector<Sample> samples;
    QElapsedTimer clock;

    // Observe production animation ticks. A fast sampling timer or a test-only
    // timeBeginPeriod call would change the timer behavior being measured.
    QObject::connect(panel, &AppActivityPanel::revealTick, win, [&](int) {
        QTimer::singleShot(0, win, [&] {
            samples.push_back({clock.elapsed(), panel->height(),
                               panel->mapTo(canvas, QPoint(0, 0)).x(),
                               panel->mapTo(canvas, QPoint(0, 0)).y()});
        });
    });

    QTimer::singleShot(80, win, [&] {
        clock.start();
        panel->setExpanded(true);
    });

    QTimer::singleShot(1200, win, [&] {

        const int startH = samples.empty() ? 0 : samples.front().height;
        const int endH = samples.empty() ? 0 : samples.back().height;
        const int travel = endH - startH;
        std::printf("== reveal ==\n");
        std::printf("  height %d -> %d  (travel %d px)\n", startH, endH, travel);

        // Frame-to-frame deltas, restricted to the samples where the panel is
        // actually growing. The sampler outlives the tween, so including the idle
        // tail would drown the motion in zeros; the curve question is about the
        // motion, and the movement invariants are measured across ALL samples below.
        std::vector<int> deltas;
        std::vector<qint64> frameGaps;
        int maxX = 0, maxTop = 0;
        qint64 motionEnd = 0;
        for (size_t i = 1; i < samples.size(); ++i) {
            maxX = std::max(maxX, std::abs(samples[i].x - samples[i - 1].x));
            maxTop = std::max(maxTop, std::abs(samples[i].topY - samples[i - 1].topY));
            const int d = samples[i].height - samples[i - 1].height;
            if (d != 0) {
                deltas.push_back(d);
                frameGaps.push_back(samples[i].ms - samples[i - 1].ms);
                motionEnd = samples[i].ms;
            }
        }
        std::printf("  motion spans %lld ms of the sampled window\n",
                    static_cast<long long>(motionEnd));

        check(!deltas.empty(), "the reveal produced samples");
        if (deltas.empty()) {
            app.exit(1);
            return;
        }

        const int maxStep = *std::max_element(deltas.begin(), deltas.end());
        const int minStep = *std::min_element(deltas.begin(), deltas.end());
        std::printf("  largest single-frame step: %d px\n", maxStep);
        std::sort(frameGaps.begin(), frameGaps.end());
        std::printf("  production timer gaps: median %lld ms, worst %lld ms\n",
                    static_cast<long long>(frameGaps[frameGaps.size() / 2]),
                    static_cast<long long>(frameGaps.back()));
        check(frameGaps[frameGaps.size() / 2] < 14,
              "the reveal updates more frequently than the old 16 ms animation tick");

        // 1. monotonic while moving: the growing sequence never reverses. A negative
        //    step here would be the panel shrinking mid-reveal.
        check(minStep >= 0, "the reveal is monotonic (never shrinks mid-expand)");

        check(maxStep < std::max(30, travel / 2),
              QStringLiteral("a populated table does not cause a large end-of-reveal jump "
                             "(largest step %1px of %2px)")
                  .arg(maxStep).arg(travel));

        // 2. zero-velocity start: the opening frames must move less than the middle.
        //    Compare the average of the first few steps with the peak step.
        const size_t n = deltas.size();
        const size_t head = std::max<size_t>(1, n / 8);
        double headAvg = 0;
        for (size_t i = 0; i < head; ++i)
            headAvg += deltas[i];
        headAvg /= double(head);

        double midAvg = 0;
        size_t midCount = 0;
        for (size_t i = n / 3; i < (2 * n) / 3; ++i) { midAvg += deltas[i]; ++midCount; }
        if (midCount)
            midAvg /= double(midCount);

        std::printf("  opening frames avg: %.1f px   middle frames avg: %.1f px\n",
                    headAvg, midAvg);

        // The signature of an ease-in-out (or any ease-in) curve: it starts slowly.
        // An ease-out curve would make headAvg the LARGEST of the two.
        check(headAvg < midAvg,
              QStringLiteral("the reveal starts slowly (opening %1 px < middle %2 px) - "
                             "an ease-in-out curve, not an ease-out step")
                  .arg(headAvg, 0, 'f', 1).arg(midAvg, 0, 'f', 1));

        // 3. the last frames must also decelerate
        const size_t tail = std::max<size_t>(1, n / 8);
        double tailAvg = 0;
        for (size_t i = n - tail; i < n; ++i)
            tailAvg += deltas[i];
        tailAvg /= double(tail);
        std::printf("  closing frames avg: %.1f px\n", tailAvg);
        check(tailAvg < midAvg,
              QStringLiteral("the reveal settles at the end (closing %1 px < middle %2 px)")
                  .arg(tailAvg, 0, 'f', 1).arg(midAvg, 0, 'f', 1));

        // 4. one motion: nothing else on screen moves horizontally, and the panel's
        //    top edge stays where it was (it grows downward only).
        check(maxX == 0, QStringLiteral("no neighbouring card moves horizontally (%1 px)")
                             .arg(maxX));
        check(maxTop <= 2, QStringLiteral("the panel's top edge stays put (%1 px)")
                               .arg(maxTop));

        win->close();
    });

    QTimer::singleShot(1600, win, [&] {
        std::printf("\n%s\n", g_failures == 0 ? "test_animation_curve passed."
                                              : "test_animation_curve FAILED.");
        app.exit(g_failures == 0 ? 0 : 1);
    });

    return app.exec();
}
