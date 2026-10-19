// Verifies that the activity table's four columns always fit inside the panel.
//
// Reported: in a restored small window the Total, Upload and Download headings are off the right
// edge and cannot be seen.
//
// The cause was the three numeric columns being QHeaderView::Fixed at widths computed from the
// widest string they can show. Fixed refuses to shrink, so in a window narrower than their
// combined demand the trailing columns were laid out past the viewport. Interactive mode lets
// them share what is available instead, and the widths are recomputed on every resize.
//
// Two things are checked, and the second matters as much as the first:
//
//   * at every width, the columns fit inside the viewport;
//   * when the table is widened again, the columns grow back - a fix that narrowed them
//     permanently would be a worse fault than the one being repaired.

#include "mainwindow.h"
#include "appactivitypanel.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
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

struct Fit {
    int viewport = 0;
    int columns = 0;
    int lastRight = 0;
    bool fits = false;
    int minColumn = 0;
};

Fit measure(QTableWidget *table)
{
    Fit f;
    f.viewport = table->viewport()->width();
    int x = 0;
    f.minColumn = 100000;
    for (int c = 0; c < table->columnCount(); ++c) {
        const int w = table->columnWidth(c);
        f.columns += w;
        x += w;
        if (w < f.minColumn)
            f.minColumn = w;
    }
    f.lastRight = x;
    f.fits = f.lastRight <= f.viewport + 1;
    return f;
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
    win->resize(1200, 800);
    win->show();
    pump(700);
    win->onPageSelected(int(Sidebar::Dashboard));
    pump(500);

    auto *panel = win->findChild<AppActivityPanel *>();
    auto *table = panel ? panel->findChild<QTableWidget *>() : nullptr;
    check(panel && table, "the panel and its table exist");
    if (!panel || !table) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    panel->setExpanded(true);
    pump(800);

    std::printf("\n== the columns across window widths ==\n");

    int worstShortfall = 0;
    int widthsTested = 0;
    for (int w : {1500, 1300, 1150, 1000, 940, 900, 860, 820, 780, 740}) {
        win->resize(w, 780);
        pump(450);
        const Fit f = measure(table);
        ++widthsTested;
        if (!f.fits)
            worstShortfall = qMax(worstShortfall, f.lastRight - f.viewport);
        std::printf("  window %5d  panel %5d  viewport %5d  columns %5d  %s\n", w,
                    panel->width(), f.viewport, f.columns,
                    f.fits ? "fits" : QStringLiteral("CLIPPED by %1").arg(f.lastRight - f.viewport).toUtf8().constData());
    }

    check(worstShortfall == 0,
          QStringLiteral("the columns fit at all %1 widths tested (worst shortfall %2px)")
              .arg(widthsTested)
              .arg(worstShortfall));

    // ---- and they grow back when there is room ---------------------------
    win->resize(760, 780);
    pump(500);
    const Fit narrow = measure(table);

    win->resize(1400, 800);
    pump(500);
    const Fit wide = measure(table);

    std::printf("\n  narrow: viewport %d  columns %d\n", narrow.viewport, narrow.columns);
    std::printf("  wide  : viewport %d  columns %d\n", wide.viewport, wide.columns);

    check(wide.columns > narrow.columns,
          QStringLiteral("widening the window gives the columns more room again "
                         "(%1px -> %2px)")
              .arg(narrow.columns)
              .arg(wide.columns));
    check(wide.fits, "and they still fit when wide");

    // A column squeezed to nothing would be unreadable even if the arithmetic added up.
    check(narrow.minColumn >= 40,
          QStringLiteral("no column is squeezed below a readable width (%1px)")
              .arg(narrow.minColumn));

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_column_fit passed." : "test_column_fit FAILED.");
    return g_failures == 0 ? 0 : 1;
}
