// Checks that the activity panel does not demand more width than the window gives it.
//
// THE FAULT
//
// With the fallback note showing, the table's columns were pushed off the right edge - in a
// full-screen window, not only a small one. The note is a sentence, and a QLabel without word
// wrap reports its whole text on one line as its size hint; a layout treats that as a minimum it
// should satisfy. So the panel's minimum width became the width of a sentence, the panel grew
// wider than the window, and the columns were laid out past the visible area.
//
// Every previous investigation looked at the column arithmetic, which was correct throughout.
// The panel it was dividing had been stretched by a caption.
//
// This checks the panel's own minimum width hint against the window, with the note visible, which
// is the condition that caused it.

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
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
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
    // The size a 1536x864 screen actually offers, which is where the fault was seen.
    win->resize(1500, 800);
    win->show();
    pump(700);
    win->onPageSelected(int(Sidebar::Dashboard));
    pump(500);

    auto *panel = win->findChild<AppActivityPanel *>();
    auto *table = panel ? panel->findChild<QTableWidget *>() : nullptr;
    check(panel && table, "the panel and table exist");
    if (!panel || !table) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    panel->setExpanded(true);
    pump(700);

    // Rows, so the table shows and is measured the way it is in use.
    table->setRowCount(5);
    for (int r = 0; r < 5; ++r) {
        table->setItem(r, 0, new QTableWidgetItem(QStringLiteral("DeepSeek Harness")));
        table->setItem(r, 1, new QTableWidgetItem(QStringLiteral("11.0 KB/s")));
        table->setItem(r, 2, new QTableWidgetItem(QStringLiteral("55.3 KB/s")));
        table->setItem(r, 3, new QTableWidgetItem(QStringLiteral("66.4 KB/s")));
    }
    table->setVisible(true);
    pump(400);

    std::printf("\n== what the panel demands ==\n");

    const int hint = panel->minimumSizeHint().width();
    const int window = win->width();
    std::printf("  window %d | panel %d | panel minimum width hint %d\n", window, panel->width(),
                hint);

    // THE CHECK. A minimum hint at or below the window means the panel can fit; above it means
    // the panel is insisting on more room than exists, which is what pushes columns off-screen.
    check(hint <= window,
          QStringLiteral("the panel does not demand more width than the window (%1 <= %2)")
              .arg(hint)
              .arg(window));

    // ---- with the longest note the panel can show -------------------------
    //
    // The note is set from the monitor's fallback reason. The one that caused the fault was the
    // long description of the kernel provider. Even shortened, the check is made against a
    // deliberately long string: the fix must not depend on the text happening to be short.
    for (QLabel *l : panel->findChildren<QLabel *>()) {
        if (l->objectName() == QLatin1String("scopeNote")) {
            l->setText(QStringLiteral(
                "Detailed per-application tracking is unavailable on this build of Windows, so "
                "the figures come from the fallback source. It counts TCP traffic; UDP and QUIC "
                "are not included. (Windows error 1450)"));
            l->setVisible(true);
            break;
        }
    }
    pump(500);

    const int hintWithLongNote = panel->minimumSizeHint().width();
    std::printf("  with the longest note: minimum %d\n", hintWithLongNote);

    check(hintWithLongNote <= window,
          QStringLiteral("and still does not, even with a long note (%1 <= %2)")
              .arg(hintWithLongNote)
              .arg(window));

    // ---- and the columns stay inside the table ----------------------------
    int columns = 0;
    for (int c = 0; c < table->columnCount(); ++c)
        columns += table->columnWidth(c);

    std::printf("  table viewport %d, columns total %d\n", table->viewport()->width(), columns);
    check(columns <= table->viewport()->width() + 1,
          "the columns fit inside the table's viewport");

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_panel_fits_width passed."
                                        : "test_panel_fits_width FAILED.");
    return g_failures == 0 ? 0 : 1;
}
