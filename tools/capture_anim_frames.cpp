// Captures the Show More / Show Less animation frame by frame, so the motion can
// be SEEN and judged, not just measured numerically.
//
// Renders the real panel inside a dashboard-like canvas, samples a frame every
// two ticks of the tween, and writes the frames to PNG files plus a contact
// sheet. Looking at the strip shows instantly what the user feels: where the
// motion bunches up, whether the end snaps, how the content below reacts.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

using namespace net;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    AppMonitor monitor;

    // A dashboard stand-in: top row, the panel, a card below - in a scroll area.
    auto *win = new QWidget;
    win->resize(760, 560);
    auto *outer = new QVBoxLayout(win);
    outer->setContentsMargins(16, 12, 16, 12);

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

    auto *top = new QWidget(canvas);
    top->setMinimumHeight(220);
    top->setObjectName(QStringLiteral("panel"));
    cl->addWidget(top);

    auto *panel = new AppActivityPanel(&monitor, canvas);
    cl->addWidget(panel);

    auto *below = new QWidget(canvas);
    below->setMinimumHeight(300);
    below->setObjectName(QStringLiteral("panel"));
    cl->addWidget(below);
    cl->addStretch(1);

    win->show();
    QApplication::processEvents();
    QApplication::processEvents();

    const int kFrames = 17;             // ~every 20ms of the 340ms tween
    int frame = 0;
    QElapsedTimer clock;

    FILE *wf = fopen("widths.tsv", "w");
    fprintf(wf, "t\tcanvas\tviewport\ttopcard\tpanel\tbelow\tpanelY\n");

    auto snap = [&](const QString &tag, int millis) {
        QImage img(win->size(), QImage::Format_ARGB32);
        img.fill(theme::kSidebar());
        win->render(&img);
        img.save(QStringLiteral("%1_%2_%3ms.png").arg(tag).arg(frame, 2, 10, QLatin1Char('0'))
                     .arg(millis), "PNG");
        fprintf(wf, "%lld\t%d\t%d\t%d\t%d\t%d\t%d\n",
                static_cast<long long>(clock.elapsed()),
                canvas->width(), scroll->viewport()->width(),
                top->width(), panel->width(), below->width(),
                panel->mapTo(canvas, QPoint(0, 0)).y());
        ++frame;
    };

    // EXPAND
    QTimer::singleShot(80, win, [&] {
        clock.start();
        panel->setExpanded(true);
        for (int i = 0; i < kFrames; ++i)
            QTimer::singleShot(i * 20, win, [&, i] { snap(QStringLiteral("exp"), clock.elapsed()); });
    });

    // COLLAPSE - with settle time so the post-glide is visible too.
    QTimer::singleShot(80 + kFrames * 20 + 900, win, [&] {
        clock.restart();
        panel->setExpanded(false);
        for (int i = 0; i < kFrames; ++i)
            QTimer::singleShot(i * 20, win, [&, i] { snap(QStringLiteral("col"), clock.elapsed()); });
    });

    QTimer::singleShot(80 + 2 * (kFrames * 20) + 1800, win, &QWidget::close);
    return app.exec();
}
