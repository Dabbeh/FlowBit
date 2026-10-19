// Verifies the panel's table geometry, which two reported bugs were about:
//
//   1. The numeric columns overflowed: "Upload" was clipped and "Total" was
//      invisible, because ResizeToContents let three columns demand more width than
//      the panel had, and QHeaderView pushed the last section past the right edge.
//
//   2. The first open was too tall. The reveal measures its target before any rows
//      exist, and the correction guard only ever RAISED the pin, so a height that
//      was pinned too large stayed wrong until a collapse/re-expand re-measured it.
//
// This asserts the geometry numerically - including the check that the rightmost
// column ends INSIDE the viewport, which is the actual failure condition. It also
// renders a PNG so the layout can be looked at.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QScrollBar>
#include <QThread>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {
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
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(1100, 800);
    auto *outer = new QVBoxLayout(win);
    auto *panel = new AppActivityPanel(&monitor, win);
    outer->addWidget(panel);
    outer->addStretch(1);
    win->show();
    QApplication::processEvents();

    std::printf("== expand, first time (the bug case) ==\n");
    panel->setExpanded(true);
    QApplication::processEvents();

    // Let the reveal run to completion, then measure.
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 700) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }

    const int firstOpenHeight = panel->height();
    std::printf("  first-open panel height: %d px\n", firstOpenHeight);

    auto *table = panel->findChild<QTableWidget *>();
    check(table != nullptr, "the panel has its application table");
    if (!table) {
        return 1;
    }

    // ---- column geometry ------------------------------------------------
    const int viewportWidth = table->viewport()->width();
    int sum = 0;
    int rightmostEnd = 0;
    for (int c = 0; c < table->columnCount(); ++c) {
        sum += table->columnWidth(c);
        rightmostEnd = qMax(rightmostEnd, table->columnViewportPosition(c)
                                               + table->columnWidth(c));
    }

    std::printf("  viewport width : %d px\n", viewportWidth);
    std::printf("  columns        :");
    for (int c = 0; c < table->columnCount(); ++c)
        std::printf(" %d", table->columnWidth(c));
    std::printf("   (sum %d)\n", sum);
    std::printf("  rightmost edge : %d px\n", rightmostEnd);

    // THE bug condition: the last column must end inside the viewport.
    check(rightmostEnd <= viewportWidth,
          QStringLiteral("all four columns fit inside the viewport "
                         "(rightmost %1 <= %2)").arg(rightmostEnd).arg(viewportWidth));

    // Every column must be wide enough to be useful, not squeezed to nothing.
    for (int c = 1; c <= 3; ++c) {
        check(table->columnWidth(c) >= 60,
              QStringLiteral("numeric column %1 has a usable width (%2 px)")
                  .arg(c).arg(table->columnWidth(c)));
    }

    // The name column must still have real room after the numeric ones.
    check(table->columnWidth(0) >= 200,
          QStringLiteral("the name column keeps the remaining room (%1 px)")
              .arg(table->columnWidth(0)));

    // The vertical scrollbar must not overlap the last column.
    if (QScrollBar *sb = table->verticalScrollBar()) {
        if (sb->isVisible()) {
            check(rightmostEnd <= viewportWidth,
                  QStringLiteral("the last column is not under the scrollbar"));
        }
    }

    // ---- the first-open height -------------------------------------------
    std::printf("\n== collapse, then open again (the reference case) ==\n");
    panel->setExpanded(false);
    clock.restart();
    while (clock.elapsed() < 700) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    panel->setExpanded(true);
    clock.restart();
    while (clock.elapsed() < 700) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    const int secondOpenHeight = panel->height();
    std::printf("  second-open panel height: %d px\n", secondOpenHeight);

    // The reported symptom was that these two differ substantially.
    const int delta = qAbs(secondOpenHeight - firstOpenHeight);
    check(delta <= 20,
          QStringLiteral("first and second open settle at the same height "
                         "(first %1, second %2, delta %3)")
              .arg(firstOpenHeight).arg(secondOpenHeight).arg(delta));

    const QPixmap shot = win->grab();
    shot.save(QStringLiteral("panel_geometry.png"), "PNG");

    std::printf("\n%s\n", g_failures == 0 ? "test_panel_geometry passed."
                                          : "test_panel_geometry FAILED.");
    return g_failures == 0 ? 0 : 1;
}
