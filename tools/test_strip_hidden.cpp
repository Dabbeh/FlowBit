// Verifies that the error strip is completely hidden when there is nothing to say.
//
// THE FAULT
//
// The strip's container carries the objectName that gives it a card background, a border and a
// red left accent. The label inside it was hidden, but the container never was - so an empty
// styled card sat on screen permanently. Reported as "the notification thing at the bottom with
// the red accent at the side... it's on all the time".
//
// This checks the CONTAINER, not the label, because the container is what is visible. A test that
// checked the label would have passed throughout the period the fault was present.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
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

// The strip's container, found the way the stylesheet finds it.
QWidget *strip(MainWindow *win)
{
    for (QWidget *w : win->findChildren<QWidget *>()) {
        if (w->objectName() == QLatin1String("errorStrip"))
            return w;
    }
    return nullptr;
}

QWidget *stripLabel(MainWindow *win)
{
    QWidget *s = strip(win);
    if (!s)
        return nullptr;
    for (QLabel *l : s->findChildren<QLabel *>()) {
        if (l->objectName() == QLatin1String("error"))
            return l;
    }
    return nullptr;
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
    win->resize(1000, 700);
    win->show();
    pump(700);
    win->onPageSelected(int(Sidebar::Dashboard));
    pump(500);

    QWidget *container = strip(win);
    QWidget *label = stripLabel(win);

    std::printf("\n== the error strip while there is nothing to say ==\n");

    check(container != nullptr, "the strip's container exists");
    if (!container) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    // The point of the whole fix: the styled card must not be on screen at rest.
    check(!container->isVisible(),
          "the container is hidden at rest, so no empty accent card is drawn");
    if (label)
        check(!label->isVisible(), "and so is the label inside it");

    // ---- raising a fault shows it ----------------------------------------
    emit engine.errorOccurred(QStringLiteral("A test fault, to make the strip appear."));
    pump(500);

    check(container->isVisible(), "raising a fault shows the container");
    if (label)
        check(label->isVisible(), "with its label");

    // ---- and it clears itself, container and all -------------------------
    std::printf("  waiting 8s for it to clear...\n");
    pump(8000);

    check(!container->isVisible(),
          "the container hides itself again, so nothing is left behind");
    if (label)
        check(!label->isVisible(), "and the label with it");

    // ---- the cross hides both, immediately --------------------------------
    emit engine.errorOccurred(QStringLiteral("A second fault, to check the cross hides both."));
    pump(500);

    QPushButton *cross = win->findChild<QPushButton *>(QStringLiteral("errorDismiss"));
    if (container->isVisible() && cross && cross->isVisible()) {
        cross->click();
        pump(400);
        check(!container->isVisible(), "the cross hides the container at once");
        if (label)
            check(!label->isVisible(), "and the label with it");
    } else {
        std::printf("  (the strip did not reappear, so the cross is not exercised)\n");
    }

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_strip_hidden passed." : "test_strip_hidden FAILED.");
    return g_failures == 0 ? 0 : 1;
}
