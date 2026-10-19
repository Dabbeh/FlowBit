// Verifies the fix for the column-heading shift.
//
// The user identified the cause precisely: when the panel opens, the table briefly
// showed its own vertical scrollbar, which then disappeared. A scrollbar occupies
// width, so its appearance narrowed the table viewport and its disappearance widened
// it again - sliding the headings ("Application / Download / Upload / Total")
// sideways, and re-measuring the whole section. Worse when maximized, because more
// rows fit and the scrollbar therefore flickers.
//
// The fix reserves the space permanently (ScrollBarAlwaysOn). This asserts the
// invariant that makes the shift impossible:
//
//   * the table viewport width does not change across the whole reveal
//   * the column x positions do not change
//   * the sum of the column widths does not change
//
// It samples before, during and after the reveal, because the flicker was a
// transient during the animation.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <windows.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QScrollArea>
#include <QScrollBar>
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

int g_failures = 0;
void check(bool ok, const QString &what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

struct Snap {
    qint64 ms;
    int viewportW;
    int col0X, col1X, col2X, col3X;
    int sum;
    bool tableVisible;
    bool scrollbarVisible;
};

void runCase(const char *label, const QSize &size)
{
    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(size);
    auto *outer = new QVBoxLayout(win);
    outer->setContentsMargins(0, 0, 0, 0);

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
    above->setMinimumHeight(300);
    above->setObjectName(QStringLiteral("panel"));
    cl->addWidget(above);

    auto *panel = new AppActivityPanel(&monitor, canvas);
    cl->addWidget(panel);

    auto *below = new QWidget(canvas);
    below->setMinimumHeight(300);
    below->setObjectName(QStringLiteral("panel"));
    cl->addWidget(below);
    cl->addStretch(1);

    win->show();
    QApplication::processEvents();

    // Populate the table so it is actually SHOWN. Without rows the panel displays
    // its empty state, the table stays hidden, and a hidden table has no viewport
    // width to measure - which made an earlier version of this test pass without
    // testing anything. injectForTesting + flushForTesting are the monitor's own
    // test hooks (the same ones test_appmon uses).
    monitor.forceRunningForTesting();
    monitor.injectForTesting(4242, QStringLiteral("firefox.exe"), 900000, 12000);
    monitor.injectForTesting(4243, QStringLiteral("steam.exe"), 400000, 3000);
    monitor.flushForTesting(1.0);
    QApplication::processEvents();

    auto *table = panel->findChild<QTableWidget *>();
    if (!table) {
        std::printf("  no table found\n");
        ++g_failures;
        return;
    }

    std::vector<Snap> snaps;
    QElapsedTimer clock;

    auto sample = [&](qint64 ms) {
        Snap s{};
        s.ms = ms;
        s.viewportW = table->viewport()->width();
        s.col0X = table->columnViewportPosition(0);
        s.col1X = table->columnViewportPosition(1);
        s.col2X = table->columnViewportPosition(2);
        s.col3X = table->columnViewportPosition(3);
        s.sum = 0;
        for (int c = 0; c < table->columnCount(); ++c)
            s.sum += table->columnWidth(c);
        s.tableVisible = table->isVisible();
        s.scrollbarVisible = table->verticalScrollBar()
                             && table->verticalScrollBar()->isVisible();
        snaps.push_back(s);
    };

    sample(-1);
    panel->setExpanded(true);
    clock.start();

    // Sample finely through the whole reveal AND the settle afterwards: the flicker
    // was transient, so a before/after comparison would miss it.
    while (clock.elapsed() < 900) {
        QApplication::processEvents(QEventLoop::AllEvents, 15);
        QThread::msleep(10);
        sample(clock.elapsed());
    }

    // Only compare samples where the table is actually shown: before the reveal it
    // has no geometry at all.
    //
    // A vacuous pass guard: if the table was NEVER visible, the ranges below stay at
    // their initial INT_MAX/0 and every assertion would succeed without measuring
    // anything. That is worse than a failure, so it is reported as one.
    const int tableVisibleSamples =
        int(std::count_if(snaps.begin(), snaps.end(),
                          [](const Snap &s) { return s.tableVisible; }));
    if (tableVisibleSamples == 0) {
        std::printf("\n  FAIL  the table was never visible, so nothing was measured\n");
        ++g_failures;
        return;
    }

    int minW = INT_MAX, maxW = 0;
    int minSum = INT_MAX, maxSum = 0;
    int minC0 = INT_MAX, maxC0 = 0, minC3 = INT_MAX, maxC3 = 0;
    int sbFlickers = 0;
    bool prevSb = snaps.front().scrollbarVisible;
    for (const Snap &s : snaps) {
        if (!s.tableVisible)
            continue;
        minW = std::min(minW, s.viewportW);
        maxW = std::max(maxW, s.viewportW);
        minSum = std::min(minSum, s.sum);
        maxSum = std::max(maxSum, s.sum);
        minC0 = std::min(minC0, s.col0X);
        maxC0 = std::max(maxC0, s.col0X);
        minC3 = std::min(minC3, s.col3X);
        maxC3 = std::max(maxC3, s.col3X);
        // Count a flicker as the scrollbar DISAPPEARING while the table is still
        // shown: that is the reported symptom (it appeared for a second, then went,
        // sliding the columns). Its one-time appearance when the table first becomes
        // visible is expected and is not counted.
        if (s.tableVisible && !s.scrollbarVisible && prevSb)
            ++sbFlickers;
        prevSb = s.scrollbarVisible;
    }

    std::printf("\n=== %s (window %dx%d) ===\n", label, size.width(), size.height());
    std::printf("  table viewport width : %d .. %d  (changed %d px)\n", minW, maxW, maxW - minW);
    std::printf("  column width sum     : %d .. %d  (changed %d px)\n", minSum, maxSum, maxSum - minSum);
    std::printf("  column 0 x           : %d .. %d\n", minC0, maxC0);
    std::printf("  column 3 x           : %d .. %d\n", minC3, maxC3);
    std::printf("  scrollbar flickers (vanished while shown): %d\n", sbFlickers);

    check(maxW - minW <= 1,
          QStringLiteral("the table viewport keeps its width (%1 px change)").arg(maxW - minW));
    check(maxSum - minSum <= 1,
          QStringLiteral("the columns keep their total width (%1 px change)").arg(maxSum - minSum));
    check(maxC3 - minC3 <= 1,
          QStringLiteral("the rightmost column does not slide (%1 px)").arg(maxC3 - minC3));
    check(sbFlickers == 0,
          QStringLiteral("the scrollbar does not vanish mid-reveal (%1 flicker(s))")
              .arg(sbFlickers));

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
    runCase("maximized-like", QSize(1920, 1040));

    std::printf("\n%s\n", g_failures == 0 ? "test_table_stability passed."
                                          : "test_table_stability FAILED.");
    timeEndPeriod(1);
    return g_failures == 0 ? 0 : 1;
}
