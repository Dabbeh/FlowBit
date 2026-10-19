// Diagnostic: reads the panel's live graphics-effect opacity while the reveal
// runs, to see whether the fade interpolates (the captures say it does not).
#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsOpacityEffect>
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
    auto *win = new QWidget;
    win->resize(700, 500);
    auto *outer = new QVBoxLayout(win);
    auto *panel = new AppActivityPanel(&monitor, win);
    outer->addWidget(panel);
    win->show();
    QApplication::processEvents();

    QElapsedTimer clock;
    QTimer *sampler = new QTimer(win);
    sampler->setInterval(20);
    QObject::connect(sampler, &QTimer::timeout, win, [&] {
        auto *fx = qobject_cast<QGraphicsOpacityEffect *>(panel->graphicsEffect());
        const double o = fx ? fx->opacity() : -1.0;
        std::printf("t=%4lldms  effect=%d  opacity=%.2f  height=%d\n",
                    clock.elapsed(), fx ? 1 : 0, o, panel->height());
    });
    sampler->start();
    clock.start();

    QTimer::singleShot(100, win, [&] { panel->setExpanded(true); });
    QTimer::singleShot(1500, win, [&] {
        sampler->stop();
        win->close();
    });
    return app.exec();
}
