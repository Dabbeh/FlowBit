// Captures the Startup card, with the settings page genuinely current.
//
// Two earlier attempts produced a stale image because they captured the window while it was
// still on the Dashboard: the page switch had not happened, and grab() faithfully rendered
// the page that was actually showing. That is a property of the harness, not of the page.
//
// This switches through the window, confirms the stack really is on the settings page,
// scrolls THAT page's own scroll area - found by walking down from the page, since the
// window holds five of them - and only then captures. Each step is printed so a wrong
// capture shows up in the output rather than only in the picture.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QThread>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

QScrollArea *ownScrollArea(QWidget *root)
{
    for (QScrollArea *a : root->findChildren<QScrollArea *>()) {
        for (QWidget *p = a->parentWidget(); p; p = p->parentWidget()) {
            if (p == root)
                return a;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1280, 820);
    win->show();
    pump(700);

    win->onPageSelected(int(Sidebar::Settings));
    pump(700);

    auto *stack = win->findChild<QStackedWidget *>();
    if (!stack) {
        std::printf("  no stack found\n");
        return 1;
    }
    std::printf("  stack index %d (Settings is %d)\n", stack->currentIndex(),
                int(Sidebar::Settings));
    if (stack->currentIndex() != int(Sidebar::Settings)) {
        std::printf("  NOT on the settings page, so the capture would be of another page\n");
        return 1;
    }

    QWidget *card = nullptr;
    for (QLabel *l : win->findChildren<QLabel *>()) {
        if (l->text() == QLatin1String("Launch Flowbit when Windows starts")) {
            QWidget *p = l->parentWidget();
            while (p && !qobject_cast<QFrame *>(p))
                p = p->parentWidget();
            card = p;
            break;
        }
    }
    if (!card) {
        std::printf("  the Startup card was not found\n");
        return 1;
    }

    if (QScrollArea *area = ownScrollArea(stack->currentWidget())) {
        int y = 0;
        for (QWidget *w = card; w && w != area->widget(); w = w->parentWidget())
            y += w->y();
        QScrollBar *bar = area->verticalScrollBar();
        bar->setValue(qMax(0, y - 24));
        pump(500);
        std::printf("  scrolled to %d of %d\n", bar->value(), bar->maximum());
    }

    const QString path = QStringLiteral("C:/Users/enaya/Desktop/Flowbit/startup-card.png");
    const QPixmap shot = win->grab();
    if (shot.save(path))
        std::printf("  wrote %s (%dx%d)\n", qPrintable(path), shot.width(), shot.height());
    else
        std::printf("  could not write %s\n", qPrintable(path));

    win->close();
    delete win;
    return 0;
}
