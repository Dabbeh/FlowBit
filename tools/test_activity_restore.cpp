// Reproduces the activity panel's clipped columns after a minimise and restore.
//
// The reported sequence: minimise the window, open it, press Show More - and the Download,
// Upload and Total headings are off the right edge.
//
// The panel is re-homed by ResponsivePair depending on width:
//
//   wide   -> a full-width card in the dashboard's canvas layout;
//   narrow -> reparented INTO the pair, which lays its children out vertically.
//
// takeMiddle() detaches it with setParent(nullptr) and the caller re-inserts it. A widget that
// has been reparented keeps its old geometry until a layout pass gives it a new one, and in the
// window that is hidden while that happens there may be no pass until it is shown again.
//
// So this runs the sequence with the window HIDDEN during the width change, which is what a
// minimise and restore actually is.

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

void report(const char *when, QWidget *panel, QTableWidget *table)
{
    int columns = 0;
    for (int c = 0; c < table->columnCount(); ++c)
        columns += table->columnWidth(c);

    std::printf("  %-26s panel %4d  table %4d  viewport %4d  columns %4d  %s\n", when,
                panel->width(), table->width(), table->viewport()->width(), columns,
                columns <= table->viewport()->width() + 1 ? "fits" : "CLIPPED");
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
    win->resize(1128, 845);
    win->show();
    pump(800);
    win->onPageSelected(int(Sidebar::Dashboard));
    pump(600);

    auto *panel = win->findChild<AppActivityPanel *>();
    auto *table = panel ? panel->findChild<QTableWidget *>() : nullptr;
    check(panel && table, "the panel and table exist");
    if (!panel || !table) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    std::printf("\n== minimise, restore, then expand ==\n");

    panel->setExpanded(true);
    pump(700);
    report("expanded, wide", panel, table);
    const int columnsWhileWide = [table] {
        int c = 0;
        for (int i = 0; i < table->columnCount(); ++i)
            c += table->columnWidth(i);
        return c;
    }();

    // ---- minimise, as the window manager does it -------------------------
    //
    // showMinimized hides the window. The resize that follows is what makes the pair stack:
    // a minimised window is given a small geometry.
    win->showMinimized();
    pump(500);

    // While hidden, the width changes - this is the case the placement logic does not cover.
    win->resize(880, 700);
    pump(500);

    // ---- restore ---------------------------------------------------------
    win->showNormal();
    pump(700);
    win->resize(1128, 845);
    pump(700);

    report("restored, expanded", panel, table);

    int columnsAfter = 0;
    for (int i = 0; i < table->columnCount(); ++i)
        columnsAfter += table->columnWidth(i);

    check(columnsAfter <= table->viewport()->width() + 1,
          QStringLiteral("all four columns fit the viewport after a minimise and restore "
                         "(%1px of %2px)")
              .arg(columnsAfter)
              .arg(table->viewport()->width()));

    check(table->viewport()->width() > 400,
          QStringLiteral("and the table has a real width rather than a column's (%1px)")
              .arg(table->viewport()->width()));

    // The panel must be back in the canvas, not stranded inside the pair.
    auto *pair = win->findChild<ResponsivePair *>();
    const bool insidePair = pair && pair->isAncestorOf(panel);
    std::printf("  panel is inside the pair: %s\n", insidePair ? "yes" : "no");
    check(!insidePair || panel->width() > 600,
          "if it is still in the pair, it is at least wide enough for its columns");

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_activity_restore passed."
                                        : "test_activity_restore FAILED.");
    return g_failures == 0 ? 0 : 1;
}
