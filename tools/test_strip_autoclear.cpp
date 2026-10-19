// Verifies that every message on the error strip clears itself.
//
// This is the SECOND time this has been wrong. The first fix added the auto-hide timer to some
// of the call sites and not others, so whether a message ever disappeared depended on which code
// path raised it - and the one that did not start the timer was the diagnostics "Report copied"
// message, which sat on screen indefinitely.
//
// The structural cause was seven places each writing out the same three lines by hand. Some
// started the timer, some did not, and nothing detected the difference. They now all call
// showErrorStrip().
//
// So the test is structural as well as behavioural: it checks that no call site shows the strip
// without the timer, which is the thing that actually went wrong. A behavioural test alone would
// pass for whichever path it happened to exercise.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
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

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    std::printf("\n== the error strip's auto-clear ==\n");

    // ---- structural: no call site shows the strip by hand -----------------
    //
    // The exact fault that recurred. If any code path bypasses showErrorStrip() it can omit the
    // timer, and nothing would report it - which is how the second occurrence happened.
    {
        QFile f(QStringLiteral("C:/Users/enaya/Desktop/Flowbit-Source/src/mainwindow.cpp"));
        if (f.open(QIODevice::ReadOnly)) {
            const QString body = QString::fromUtf8(f.readAll());
            const int handWritten = body.count(QStringLiteral("m_errorLabel->show();"));
            const int routed = body.count(QStringLiteral("showErrorStrip();"));

            std::printf("  call sites: %d routed through showErrorStrip(), %d hand-written\n",
                        routed, handWritten);

            check(handWritten == 0,
                  QStringLiteral("no call site shows the strip by hand (%1 found)").arg(handWritten));
            check(routed >= 5,
                  QStringLiteral("and the known call sites are routed (%1)").arg(routed));
        } else {
            std::printf("  (source not readable, structural check skipped)\n");
        }
    }

    // ---- behavioural: a message raised on the strip goes away -------------
    QTemporaryDir dir;
    static SettingsStore store(dir.path() + QStringLiteral("/cfg"));
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1000, 700);
    win->show();
    pump(600);
    win->onPageSelected(int(Sidebar::Dashboard));
    pump(400);

    // Raise a fault through the engine's own signal, which is a real path.
    emit engine.errorOccurred(QStringLiteral("A test fault raised to check the strip clears."));
    pump(500);

    QLabel *strip = nullptr;
    for (QLabel *l : win->findChildren<QLabel *>()) {
        if (l->objectName() == QLatin1String("error") && l->isVisible() && !l->text().isEmpty()) {
            strip = l;
            break;
        }
    }

    check(strip != nullptr, "raising a fault shows the strip");
    if (strip) {
        std::printf("  the strip reads: \"%s\"\n", qPrintable(strip->text().left(50)));

        // The timer is 6s plus a 400ms fade. Waiting longer than both, because the check is that
        // it clears, not that it clears by a particular millisecond.
        std::printf("  waiting 8s for it to clear...\n");
        pump(8000);

        check(!strip->isVisible(),
              "and it clears itself without the user pressing anything");
    }

    // ---- and the dismiss cross still works --------------------------------
    emit engine.errorOccurred(QStringLiteral("A second fault, to check the cross."));
    pump(500);

    QPushButton *cross = win->findChild<QPushButton *>(QStringLiteral("errorDismiss"));
    if (strip && cross && strip->isVisible() && cross->isVisible()) {
        cross->click();
        pump(300);
        check(!strip->isVisible(), "the cross hides it immediately");
    } else {
        std::printf("  (the strip was not shown again, so the cross is not exercised)\n");
    }

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_strip_autoclear passed."
                                        : "test_strip_autoclear FAILED.");
    return g_failures == 0 ? 0 : 1;
}
