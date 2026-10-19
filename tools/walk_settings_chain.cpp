// Reports the chain from the settings body down to the diagnostics panel, with each
// level's geometry, so the reason the DIAGNOSTICS card is not drawn can be read.
//
// The panel measures 1267x732 and its contents fit, yet the card does not appear in a
// rendered screenshot - the page stops after APPLICATION. So the fault is above the
// panel: either the card has no height, or it sits below the scroll area's content.
// Every attempt so far has reasoned about the panel; this walks outward instead.

#include "mainwindow.h"
#include "diagnosticspanel.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

void walk(const QSize &size)
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
    std::printf("\n=== window %dx%d ===\n", size.width(), size.height());
    if (!panel) {
        std::printf("  no panel\n");
        return;
    }

    // Panel -> outward. y is relative to the parent, which is what shows whether a card
    // is being laid out off the bottom of its container.
    std::printf("  %-28s %6s %6s %6s %6s  %s\n",
                "widget", "w", "h", "y", "vis", "sizeHint h");
    for (QWidget *w = panel; w; w = w->parentWidget()) {
        std::printf("  %-28s %6d %6d %6d %6s  %d\n",
                    w->metaObject()->className(), w->width(), w->height(), w->y(),
                    w->isVisible() ? "yes" : "no", w->sizeHint().height());
        if (w == win)
            break;
    }

    // The card that holds the panel, and the layout that places cards.
    if (QWidget *card = panel->parentWidget()) {
        std::printf("\n  card is %s, objectName='%s'\n",
                    card->metaObject()->className(), qPrintable(card->objectName()));
        if (auto *cl = qobject_cast<QVBoxLayout *>(card->layout())) {
            std::printf("  card layout: %d items, hint h=%d, min h=%d\n",
                        cl->count(), cl->sizeHint().height(), cl->minimumSize().height());
            for (int i = 0; i < cl->count(); ++i) {
                QLayoutItem *it = cl->itemAt(i);
                QWidget *cw = it ? it->widget() : nullptr;
                std::printf("    item %d: %-24s h=%-6d y=%-6d minH=%d hintH=%d\n",
                            i, cw ? cw->metaObject()->className() : "(layout)",
                            cw ? cw->height() : -1, cw ? cw->y() : -1,
                            it ? it->minimumSize().height() : -1,
                            it ? it->sizeHint().height() : -1);
            }
        }
    }

    // The scroll area: is the content taller than the viewport, and by how much?
    for (QScrollArea *sa : win->findChildren<QScrollArea *>()) {
        if (!sa->isVisible())
            continue;
        QWidget *content = sa->widget();
        std::printf("\n  scroll area %dx%d  viewport h=%d  content h=%d  range 0..%d\n",
                    sa->width(), sa->height(), sa->viewport()->height(),
                    content ? content->height() : -1,
                    sa->verticalScrollBar()->maximum());
        if (content && content->layout()) {
            std::printf("    content layout: %d items, hint h=%d, min h=%d\n",
                        content->layout()->count(), content->layout()->sizeHint().height(),
                        content->layout()->minimumSize().height());
        }
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
    walk(QSize(1536, 816));
    return 0;
}
