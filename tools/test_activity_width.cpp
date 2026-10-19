// Reproduces the layout fault in the Application Network Activity panel.
//
// Reported: minimise the window, open it again, press Show More - and the Total, Upload and
// Download columns are pushed off the right edge so they cannot be read.
//
// The suspicion is the placement logic in MainWindow, not the table. The panel is moved between
// two homes depending on the window's width:
//
//   wide   -> a full-width card directly beneath the top row;
//   narrow -> embedded inside the ResponsivePair, between Live Speed and Usage.
//
// That move happens on ResponsivePair::orientationChanged. If the panel is HIDDEN when the
// orientation changes, the move still happens but the layout it lands in is sized for a column
// rather than for the window - and nothing puts it right later, because the orientation does
// not change again.
//
// So this drives the exact sequence: show, resize narrow, hide, resize wide, show again.

#include "mainwindow.h"
#include "appactivitypanel.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "responsivepair.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

// The width available to the table, and whether the last column fits inside it.
struct TableFit {
    int tableWidth = 0;
    int viewportWidth = 0;
    int columnsTotal = 0;
    int lastColumnRight = 0;
    bool lastColumnVisible = false;
};

TableFit measure(QTableWidget *table)
{
    TableFit f;
    if (!table)
        return f;

    f.tableWidth = table->width();
    f.viewportWidth = table->viewport()->width();

    for (int c = 0; c < table->columnCount(); ++c)
        f.columnsTotal += table->columnWidth(c);

    if (table->columnCount() > 0) {
        int x = 0;
        for (int c = 0; c < table->columnCount() - 1; ++c)
            x += table->columnWidth(c);
        f.lastColumnRight = x + table->columnWidth(table->columnCount() - 1);
    }

    // The last column is readable when its right edge is inside the viewport.
    f.lastColumnVisible = f.lastColumnRight <= f.viewportWidth + 1;
    return f;
}

void report(const char *when, const TableFit &f)
{
    std::printf("  %-22s table %4d  viewport %4d  columns %4d  right edge %4d  %s\n", when,
                f.tableWidth, f.viewportWidth, f.columnsTotal, f.lastColumnRight,
                f.lastColumnVisible ? "fits" : "CLIPPED");
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    QTemporaryDir dir;
    static SettingsStore store(dir.path() + QStringLiteral("/cfg"));
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    // Wide, matching the maximised window in the report.
    win->resize(1536, 816);
    win->show();
    pump(700);
    win->onPageSelected(int(Sidebar::Dashboard));
    pump(500);

    auto *panel = win->findChild<AppActivityPanel *>();
    check(panel != nullptr, "the activity panel exists");
    if (!panel) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    auto *table = panel->findChild<QTableWidget *>();
    check(table != nullptr, "it holds the activity table");
    if (!table) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    std::printf("\n== the reported sequence ==\n");

    // ---- step 1: expanded while wide -------------------------------------
    // The panel is opened by the Show More button on the Live Speed card.
    panel->setExpanded(true);
    pump(600);
    report("wide, expanded", measure(table));
    const TableFit wide = measure(table);
    check(wide.lastColumnVisible, "expanded while wide: the Total column is readable");

    // ---- step 2: narrow, so the panel is re-homed ------------------------
    // Below the pair's 940px threshold it embeds inside the pair, where there is only a
    // column's width for the table.
    win->resize(880, 700);
    pump(700);
    report("narrow, expanded", measure(table));

    // ---- step 3: collapse and widen --------------------------------------
    // This is the reported sequence: minimise (which the window reports as a resize), open
    // again, then expand. The collapse while narrow, followed by a widening, is what leaves
    // the placement stale.
    panel->setExpanded(false);
    pump(500);
    win->resize(1536, 816);
    pump(700);

    // ---- step 4: expand again at full width ------------------------------
    panel->setExpanded(true);
    pump(800);
    const TableFit after = measure(table);
    report("wide, re-expanded", after);

    check(after.lastColumnVisible,
          QStringLiteral("after narrowing, widening and re-expanding: the Total column is "
                         "still readable (%1px past the viewport)")
              .arg(after.lastColumnRight - after.viewportWidth));

    // The panel should be back at the window's full width, not the pair's column width.
    check(panel->width() >= win->width() / 2,
          QStringLiteral("and the panel is full width again (%1px of a %2px window)")
              .arg(panel->width())
              .arg(win->width()));

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_activity_width passed."
                                        : "test_activity_width FAILED.");
    return g_failures == 0 ? 0 : 1;
}
