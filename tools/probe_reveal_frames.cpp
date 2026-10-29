// Measures the reveal: frame pacing AND how much work each frame causes.
//
// Qt drives every animation from one global timer, so a reveal can only be smooth if the work per
// frame is small. Two things are recorded:
//
//   * the interval between reveal ticks - long gaps are dropped frames
//   * how many times the TABLE is resized during the reveal - a resize means the layout re-ran,
//     which is the expensive kind of per-frame work (every row, every column re-measured)
//
// If the table resizes on most frames, the animation is paying for a full relayout each tick,
// which is what "not smooth" looks like from the inside.

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
#include <QEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <cstdio>

using namespace net;

namespace {

void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(2); }
}

// Counts Resize events reaching the table.
struct ResizeCounter : QObject {
    int count = 0;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Resize) ++count;
        return QObject::eventFilter(o, e);
    }
};

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
    pump(600);
    win->onPageSelected(int(Sidebar::Dashboard));
    pump(400);

    auto *panel = win->findChild<AppActivityPanel *>();
    auto *table = panel ? panel->findChild<QTableWidget *>() : nullptr;
    if (!panel || !table) { std::printf("  panel/table missing\n"); return 1; }

    // rows so the reveal has real content
    table->setRowCount(6);
    for (int r = 0; r < 6; ++r) {
        table->setItem(r, 0, new QTableWidgetItem(QStringLiteral("application-%1").arg(r)));
        for (int c = 1; c <= 3; ++c)
            table->setItem(r, c, new QTableWidgetItem(QStringLiteral("1.42 MB/s")));
    }

    ResizeCounter counter;
    table->installEventFilter(&counter);

    QVector<int> heights;
    QVector<qint64> stamps;
    QElapsedTimer clock; clock.start();

    QObject::connect(panel, &AppActivityPanel::revealTick, [&](int h) {
        heights << h;
        stamps << clock.elapsed();
    });

    counter.count = 0;
    panel->setExpanded(true);
    pump(700);

    std::printf("\n== reveal frames ==\n");
    std::printf("  frames sampled      : %d\n", heights.size());
    std::printf("  table resizes       : %d  (one per frame means the layout re-ran)\n", counter.count);
    if (heights.size() > 2) {
        std::printf("  first/last height   : %d -> %d\n", heights.first(), heights.last());
        QVector<qint64> gaps;
        for (int i = 1; i < stamps.size(); ++i) gaps << (stamps[i] - stamps[i-1]);
        std::sort(gaps.begin(), gaps.end());
        const qint64 median = gaps[gaps.size()/2];
        const qint64 worst = gaps.last();
        int over20 = 0, over33 = 0;
        for (qint64 g : gaps) { if (g > 20) ++over20; if (g > 33) ++over33; }
        std::printf("  frame gap median    : %lld ms\n", static_cast<long long>(median));
        std::printf("  frame gap worst     : %lld ms\n", static_cast<long long>(worst));
        std::printf("  frames over 20ms    : %d of %d\n", over20, gaps.size());
        std::printf("  frames over 33ms    : %d of %d  (visible stutter)\n", over33, gaps.size());

        // WHERE the long frames are matters: a stall on the first frame is setup cost, a stall in
        // the middle is something else competing with the animation.
        std::printf("  long-frame positions:");
        for (int i = 0; i < gaps.size(); ++i)
            if (gaps[i] > 20) std::printf(" #%d(%lldms)", i + 1, static_cast<long long>(gaps[i]));
        std::printf("\n");
        std::printf("  first gap           : %lld ms\n", static_cast<long long>(gaps.first()));
        std::printf("  last gap            : %lld ms\n", static_cast<long long>(gaps.last()));
    }

    win->close();
    delete win;
    return 0;
}
