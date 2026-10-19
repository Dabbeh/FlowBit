// Dumps the diagnostics grid as Qt itself sees it: every row, every column, whether a
// cell holds an item, and the widget's own geometry inside that cell.
//
// The measurements so far have been confusing in a specific way: the grid reports 26
// rows and only the first two have any height, and only 15 rows are printed with
// content. Those two facts together suggest the items are not where the row indices
// assume - which would be my own bug in buildRows(), because the row counter advances
// per widget while the group heading spans all three columns.
//
// This prints what is actually at each position instead of assuming.

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

void dump(const QSize &size)
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
    auto *grid = panel ? panel->findChild<QGridLayout *>() : nullptr;
    std::printf("\n=== window %dx%d  panel %dx%d ===\n",
                size.width(), size.height(), panel ? panel->width() : 0,
                panel ? panel->height() : 0);
    if (!grid) {
        std::printf("  no grid\n");
        return;
    }

    std::printf("  grid count=%d rows=%d cols=%d\n",
                grid->count(), grid->rowCount(), grid->columnCount());
    std::printf("  grid geometry %dx%d\n", grid->geometry().width(), grid->geometry().height());

    for (int i = 0; i < grid->count(); ++i) {
        QLayoutItem *it = grid->itemAt(i);
        if (!it)
            continue;
        int r = -1, c = -1, rs = -1, cs = -1;
        grid->getItemPosition(i, &r, &c, &rs, &cs);
        QWidget *w = it->widget();
        QString text;
        if (auto *l = qobject_cast<QLabel *>(w))
            text = l->text().left(38);
        std::printf("  item %2d -> r%-3d c%-2d span %dx%d  widget %s  geom %dx%d  hint %dx%d  %s\n",
                    i, r, c, rs, cs,
                    w ? w->metaObject()->className() : "(spacer)",
                    w ? w->width() : 0, w ? w->height() : 0,
                    w ? w->sizeHint().width() : 0, w ? w->sizeHint().height() : 0,
                    qPrintable(text));
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
    dump(QSize(1536, 816));
    return 0;
}
