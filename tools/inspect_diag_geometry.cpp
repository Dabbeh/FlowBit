// Prints the diagnostics panel's internal geometry, so the collapse is measured rather
// than inferred from a picture.
//
// A rendered screenshot shows the rows crushed to a few pixels with a large empty gap
// below them. Several layout changes have been made; none has been shown to fix it.
// This reports, per grid row: the row's height, and the heights of the widgets in it.
// A row whose height is a few pixels while its widgets want more says the grid is not
// being given space; tall rows with short widgets would say the opposite.

#include "mainwindow.h"
#include "diagnosticspanel.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QThread>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

void inspect(const char *label, const QSize &size)
{
    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(size);
    win->show();
    QApplication::processEvents();
    for (int i = 0; i < 15; ++i) { QApplication::processEvents(); QThread::msleep(10); }

    if (auto *side = win->findChild<Sidebar *>())
        win->onPageSelected(int(Sidebar::Settings));
    for (int i = 0; i < 30; ++i) { QApplication::processEvents(); QThread::msleep(10); }

    auto *panel = win->findChild<DiagnosticsPanel *>();
    std::printf("\n=== %s  window %dx%d ===\n", label, size.width(), size.height());
    if (!panel) {
        std::printf("  panel not found\n");
        return;
    }

    std::printf("  panel %dx%d  sizeHint %dx%d  minimumSizeHint %dx%d  policy v=%d\n",
                panel->width(), panel->height(),
                panel->sizeHint().width(), panel->sizeHint().height(),
                panel->minimumSizeHint().width(), panel->minimumSizeHint().height(),
                int(panel->sizePolicy().verticalPolicy()));

    // Every label inside, with the height it wants versus the height it got.
    int shortLabels = 0;
    const auto labels = panel->findChildren<QLabel *>();
    std::printf("  labels: %d\n", int(labels.size()));
    for (QLabel *l : labels) {
        const int want = l->sizeHint().height();
        const int got = l->height();
        if (!l->text().isEmpty() && got < want - 1) {
            ++shortLabels;
            if (shortLabels <= 8) {
                std::printf("    CLIPPED: want h=%d got h=%d  \"%s\"\n",
                            want, got, qPrintable(l->text().left(50)));
            }
        }
    }
    std::printf("  labels shorter than their own hint: %d\n", shortLabels);

    // The grid itself.
    if (auto *grid = panel->findChild<QGridLayout *>()) {
        std::printf("  grid: %d rows x %d cols, geometry %dx%d, sizeHint %dx%d\n",
                    grid->rowCount(), grid->columnCount(),
                    grid->geometry().width(), grid->geometry().height(),
                    grid->sizeHint().width(), grid->sizeHint().height());
        for (int r = 0; r < qMin(16, grid->rowCount()); ++r) {
            const QRect cell = grid->cellRect(r, 2);
            QLayoutItem *it = grid->itemAtPosition(r, 2);
            QString what;
            if (it && it->widget()) {
                QLabel *l = qobject_cast<QLabel *>(it->widget());
                what = l ? l->text().left(40) : QStringLiteral("(widget)");
            }
            std::printf("    gridrow %2d: cell h=%-4d w=%-5d rowMinH=%-4d  %s\n",
                        r, cell.height(), cell.width(), grid->rowMinimumHeight(r),
                        qPrintable(what));
        }
    } else {
        std::printf("  NO GRID found in the panel\n");
    }

    win->close();
    delete win;
    QApplication::processEvents();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    inspect("maximized", QSize(1536, 816));
    inspect("narrow", QSize(900, 700));
    return 0;
}
