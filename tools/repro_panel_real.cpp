// Reproduces the Show Less crash against the REAL AppActivityPanel.
//
// Four isolated theories were disproved, so this stops simulating the panel and
// uses the actual class from the application, driven through the same sequence the
// user performs: expand, collapse, repeatedly and at short intervals, including
// toggles that interrupt an animation already running.
//
// If this crashes, the stack trace points at the real defect. If it does not, the
// crash needs something the test environment lacks (real platform plugin, a sized
// window, the scroll area) and the dump route in capture-crash-trace.ps1 is needed.
//
// Run with QT_QPA_PLATFORM unset so the real windows platform plugin is used.

#include "appactivitypanel.h"
#include "appmonitor.h"
#include "theme.h"

#include <QApplication>
#include <QDateTime>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

int g_toggles = 0;

// Finds the panel's own toggle buttons, the way the test suite does.
QPushButton *findButton(QWidget *root, const QString &prefix, bool excludePanelDescendants,
                        AppActivityPanel *panel)
{
    const QList<QPushButton *> all = root->findChildren<QPushButton *>();
    for (QPushButton *b : all) {
        if (!b->text().startsWith(prefix))
            continue;
        if (excludePanelDescendants && panel && panel->isAncestorOf(b))
            continue;
        return b;
    }
    return nullptr;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);

    AppMonitor monitor;

    auto *win = new QWidget;
    win->resize(900, 700);
    auto *outer = new QVBoxLayout(win);

    // Mirror the dashboard: the panel lives inside a scroll area, which is where
    // the application puts it.
    auto *scroll = new QScrollArea(win);
    scroll->setWidgetResizable(true);
    auto *canvas = new QWidget;
    auto *canvasLayout = new QVBoxLayout(canvas);
    scroll->setWidget(canvas);
    outer->addWidget(scroll);

    // Something above the panel so it is not at y=0.
    auto *spacer = new QWidget(canvas);
    spacer->setMinimumHeight(300);
    canvasLayout->addWidget(spacer);

    auto *panel = new AppActivityPanel(&monitor, canvas);
    canvasLayout->addWidget(panel);
    canvasLayout->addStretch(1);

    // A Show More button outside the panel, like the dashboard's.
    auto *showMore = new QPushButton(QStringLiteral("Show More"), win);
    outer->addWidget(showMore);
    QObject::connect(showMore, &QPushButton::clicked, win, [panel] {
        panel->setExpanded(!panel->isExpanded());
    });

    win->show();

    // Drive the panel directly, hard: 80 toggles at 120 ms, so many of them
    // interrupt an animation still in flight (280 ms long).
    for (int i = 0; i < 80; ++i) {
        QTimer::singleShot(300 + i * 120, win, [panel, &i] {
            panel->setExpanded(i % 2 == 0);
            ++g_toggles;
        });
    }

    // Also move the panel between layouts mid-animation, the way the responsive
    // top row does when the window crosses its breakpoint.
    QTimer::singleShot(300 + 40 * 120, win, [panel, canvas] {
        panel->setParent(nullptr);
        panel->setParent(canvas);
        panel->setVisible(true);
        canvas->layout()->addWidget(panel);
    });

    QTimer::singleShot(300 + 80 * 120 + 800, win, [win] {
        std::printf("SURVIVED %d toggles plus a re-parent\n", g_toggles);
        win->close();
    });

    const int rc = app.exec();
    std::printf("exit code %d after %d toggles\n", rc, g_toggles);
    return rc;
}
