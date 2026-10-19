// Pinpoints which step of the Show More click blocks, by printing a marker
// before and after every operation with an immediate flush.
#include "../src/mainwindow.h"
#include "../src/appactivitypanel.h"
#include "../src/appmonitor.h"
#include "../src/settingsstore.h"
#include "../src/speedmonitor.h"
#include "../src/throttleengine.h"
#include "../src/usagetracker.h"

#include <QApplication>
#include <QDir>
#include <QPushButton>
#include <QScrollArea>

#include <cstdio>

using namespace net;

#define MARK(msg)                                                                 \
    do {                                                                          \
        std::printf("MARK %s\n", msg);                                            \
        std::fflush(stdout);                                                      \
    } while (0)

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    MARK("app");

    const QString scratch = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("bisect_scratch"));
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    SettingsStore store(scratch);
    ThrottleEngine engine;
    SpeedMonitor monitor;
    UsageTracker usage;
    usage.setDataDirectory(scratch);

    MARK("construct");
    MainWindow window(&store, &engine, &monitor, &usage);
    window.resize(900, 700);
    window.show();
    QApplication::processEvents();
    MARK("shown");

    AppActivityPanel *panel = window.findChild<AppActivityPanel *>();
    QPushButton *btn = nullptr;
    for (QPushButton *b : window.findChildren<QPushButton *>()) {
        if (b->text().startsWith(QStringLiteral("Show More"))) {
            btn = b;
            break;
        }
    }
    MARK("found widgets");

    if (!panel || !btn) {
        std::printf("missing widgets\n");
        return 1;
    }

    btn->click();
    MARK("clicked - if hang, it is inside click");
    QApplication::processEvents();
    MARK("processed events");

    AppMonitor *mon = window.findChild<AppMonitor *>();
    std::printf("panel visible=%d expanded=%d monitorState=%d err=%s\n",
                int(panel->isVisible()), int(panel->isExpanded()),
                mon ? int(mon->state()) : -1,
                mon ? qPrintable(mon->errorString()) : "(none)");
    std::fflush(stdout);

    window.close();
    QDir(scratch).removeRecursively();
    return 0;
}
