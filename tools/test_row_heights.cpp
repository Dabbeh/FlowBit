// Verifies BOTH requirements at once, which the last two attempts satisfied only one
// at a time:
//
//   1. The two cards in the top row must end up THE SAME HEIGHT. A screenshot showed
//      the 30-Day Usage card squashed beside a full-height Live Speed card after an
//      attempt that clamped the row's height; an earlier attempt pinning the cards to
//      Fixed caused the same thing.
//
//   2. Nothing may oscillate while the panel animates. Measured previously at the
//      user's real window size (1536x816 logical = 1920x1080 at 125% scaling): the
//      Usage card flipped between 271 and ~258 several times a second, including after
//      the reveal finished, and each flip moved the graph.
//
// The two are in tension - clamping satisfies (2) but breaks (1); letting everything
// stretch satisfies (1) but can restore the loop - so this asserts both, in both
// orientations, and reports the actual heights rather than a verdict alone.

#include "mainwindow.h"
#include "appactivitypanel.h"
#include "responsivepair.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "theme.h"

#include <windows.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <vector>

using namespace net;

namespace {

int g_failures = 0;
void check(bool ok, const QString &what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

struct Snap {
    qint64 ms;
    int pairH;
    int aH, bH;
    int aY, bY;
    int canvasH;
};

void runCase(const char *label, const QSize &size)
{
    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor speed;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &speed, &usage);
    win->resize(size);
    win->show();
    QApplication::processEvents();
    QApplication::processEvents();

    auto *panel = win->findChild<AppActivityPanel *>();
    auto *pair = win->findChild<ResponsivePair *>();
    if (!panel || !pair) {
        std::printf("\n=== %s === widgets not found\n", label);
        win->close();
        delete win;
        return;
    }

    QWidget *a = nullptr;
    QWidget *b = nullptr;
    for (QWidget *k : pair->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!k->isWidgetType())
            continue;
        if (!a) a = k;
        else if (!b) b = k;
    }

    std::vector<Snap> snaps;
    QElapsedTimer clock;

    auto sample = [&](qint64 ms) {
        Snap s{};
        s.ms = ms;
        s.pairH = pair->height();
        s.aH = a ? a->height() : 0;
        s.bH = b ? b->height() : 0;
        s.aY = a ? a->y() : 0;
        s.bY = b ? b->y() : 0;
        s.canvasH = pair->parentWidget() ? pair->parentWidget()->height() : 0;
        snaps.push_back(s);
    };

    sample(-1);
    panel->setExpanded(true);
    clock.start();
    while (clock.elapsed() < 1200) {
        QApplication::processEvents(QEventLoop::AllEvents, 6);
        QThread::msleep(6);
        sample(clock.elapsed());
    }

    auto range = [&](int Snap::*f) {
        int lo = INT_MAX, hi = INT_MIN;
        for (const Snap &s : snaps) { lo = std::min(lo, s.*f); hi = std::max(hi, s.*f); }
        return std::make_pair(lo, hi);
    };

    auto [aHlo, aHhi] = range(&Snap::aH);
    auto [bHlo, bHhi] = range(&Snap::bH);
    auto [pairLo, pairHi] = range(&Snap::pairH);

    const Snap &f = snaps.front();
    std::printf("\n=== %s (window %dx%d) ===\n", label, size.width(), size.height());
    std::printf("  orientation : %s\n", pair->isStacked() ? "STACKED" : "side by side");
    std::printf("  card A height : %d .. %d\n", aHlo, aHhi);
    std::printf("  card B height : %d .. %d\n", bHlo, bHhi);
    std::printf("  pair height   : %d .. %d\n", pairLo, pairHi);

    // Requirement 1: in side-by-side the two cards must match.
    if (!pair->isStacked()) {
        const int diff = std::abs(aHhi - bHhi);
        std::printf("  heights at end: A=%d  B=%d  (difference %d)\n", aHhi, bHhi, diff);
        check(diff <= 1,
              QStringLiteral("the two cards are the same height in one row "
                             "(A %1, B %2)").arg(aHhi).arg(bHhi));
    }

    // Requirement 2: nothing oscillates.
    const int aMove = aHhi - aHlo;
    const int bMove = bHhi - bHlo;
    const int pairMove = pairHi - pairLo;
    std::printf("  movement during reveal: A %d px, B %d px, pair %d px\n",
                aMove, bMove, pairMove);
    check(aMove <= 1, QStringLiteral("card A holds its height (%1 px)").arg(aMove));
    check(bMove <= 1, QStringLiteral("card B holds its height (%1 px)").arg(bMove));

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

    // The real geometry for this machine: 1920x1080 at 125% scaling.
    runCase("maximized (REAL 1536x816)", QSize(1536, 816));
    runCase("narrow -> stacked", QSize(900, 780));
    runCase("half screen -> stacked", QSize(768, 780));

    std::printf("\n%s\n", g_failures == 0 ? "test_row_heights passed."
                                          : "test_row_heights FAILED.");
    timeEndPeriod(1);
    return g_failures == 0 ? 0 : 1;
}
