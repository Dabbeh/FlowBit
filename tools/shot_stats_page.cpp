// Renders the real Statistics page in a short window and writes a PNG, so the layout
// can be seen rather than inferred from numbers.
//
// The reported problem was visual: in a small window the Usage by Application table
// showed a single row and the rest was unreachable. Measurements alone have been
// misleading here - a series of tests reported identical table and scroll numbers at
// every window size, which no real layout can produce, so something about the
// measurement was wrong and the picture is the honest check.
//
// Also prints the geometry as it goes, so the image can be read against the numbers.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "theme.h"
#include "sidebar.h"

#include <QApplication>
#include <QHeaderView>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

quint64 mb(double n) { return quint64(n * 1024.0 * 1024); }

void shoot(const char *name, const QSize &size)
{
    QTemporaryDir dir;
    static UsageTracker tracker;
    tracker.setDataDirectory(dir.path());
    static SettingsStore store;

    Settings settings = store.load();
    tracker.load(settings.usage);
    for (int i = 1; i <= 12; ++i) {
        tracker.addAppSample(QStringLiteral("C:/Apps/app%1.exe").arg(i),
                             QStringLiteral("Application %1").arg(i),
                             mb(500.0 - i * 20), mb(50));
    }
    tracker.flush();

    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    auto *win = new MainWindow(&store, &engine, &monitor, &tracker);
    win->resize(size);
    win->show();
    QApplication::processEvents();
    for (int i = 0; i < 15; ++i) { QApplication::processEvents(); QThread::msleep(10); }

    // Switch to the Statistics page exactly as the sidebar does.
    if (auto *side = win->findChild<Sidebar *>())
        win->onPageSelected(int(Sidebar::Statistics));
    for (int i = 0; i < 20; ++i) { QApplication::processEvents(); QThread::msleep(10); }

    QTableWidget *table = nullptr;
    for (QTableWidget *t : win->findChildren<QTableWidget *>()) {
        if (t->columnCount() == 4 && t->horizontalHeaderItem(0)
            && t->horizontalHeaderItem(0)->text() == QLatin1String("Application")) {
            table = t;
            break;
        }
    }

    std::printf("\n=== %s  window %dx%d ===\n", name, size.width(), size.height());
    if (table) {
        std::printf("  table %dx%d  rows %d  visible=%s\n",
                    table->width(), table->height(), table->rowCount(),
                    table->isVisible() ? "yes" : "no");
        std::printf("  columns:");
        for (int c = 0; c < table->columnCount(); ++c)
            std::printf(" %d", table->columnWidth(c));
        std::printf("\n  viewport %dx%d\n",
                    table->viewport()->width(), table->viewport()->height());
    } else {
        std::printf("  table NOT FOUND\n");
    }

    for (QScrollArea *sa : win->findChildren<QScrollArea *>()) {
        if (sa->isVisible() && sa->verticalScrollBar()->maximum() > 0) {
            std::printf("  visible scroll area: %dx%d  range 0..%d  content h %d\n",
                        sa->width(), sa->height(), sa->verticalScrollBar()->maximum(),
                        sa->widget() ? sa->widget()->height() : 0);
        }
    }

    const QString path = QStringLiteral("C:/Users/enaya/Desktop/Flowbit/stats-%1.png").arg(name);
    const QPixmap shot = win->grab();
    if (shot.save(path))
        std::printf("  wrote %s (%dx%d)\n", qPrintable(path), shot.width(), shot.height());
    else
        std::printf("  FAILED to write %s\n", qPrintable(path));

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

    shoot("short", QSize(900, 560));
    shoot("max", QSize(1536, 816));
    return 0;
}
