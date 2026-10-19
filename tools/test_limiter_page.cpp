// Verifies the Limiter page reports the truth when it is opened.
//
// Reported symptom: limits were enabled and saved, but the page still read "Status: Off"
// and "Uncapped". Cause: the page-switch handler refreshed Statistics, Scheduler and
// Settings but had no case for Sidebar::Limiter, and its labels are written only by
// refreshStatus() - so they kept whatever was true when the application started.
//
// This drives the real MainWindow: it enables the limiter, visits another page, then
// opens the Limiter page and asserts the labels agree with the settings.

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

// The Limiter page's status value, found by its object name.
QLabel *findStatValueWithText(QWidget *root, const QString &prefix)
{
    for (QLabel *l : root->findChildren<QLabel *>()) {
        if (l->objectName() == QLatin1String("statValue")
            && l->text().startsWith(prefix))
            return l;
    }
    return nullptr;
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
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

    // Enable the limiter and set real limits, the way the Dashboard controls would.
    Settings settings = store.load();
    settings.enabled = true;
    settings.defaultDown = 1184891;   // ~1.13 MB/s
    settings.defaultUp = 524288;      // ~512 KB/s
    settings.rules.clear();           // no schedule, so the manual limits apply
    settings.appRules.clear();
    QString err;
    store.save(settings, &err);

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1100, 860);
    win->show();
    pump(300);

    auto *side = win->findChild<Sidebar *>();

    std::printf("\n== the limiter page after visiting another page ==\n");

    // Start on the Dashboard so the Limiter page has not been seen, then leave and
    // return - which is the sequence that used to show stale text.
    if (side)
        win->onPageSelected(int(Sidebar::Dashboard));
    pump(200);
    if (side)
        win->onPageSelected(int(Sidebar::Statistics));
    pump(200);
    if (side)
        win->onPageSelected(int(Sidebar::AppLimiter));
    pump(400);

    QLabel *status = findStatValueWithText(win, QStringLiteral("Status:"));
    check(status != nullptr, "the limiter page has a status label");
    if (status) {
        const QString text = status->text();
        std::printf("  status text: \"%s\"\n", qPrintable(text));
        check(!text.contains(QLatin1String("Off")),
              QStringLiteral("the status is NOT \"Off\" while the limiter is enabled (%1)")
                  .arg(text));
        check(text.contains(QLatin1String("Active")),
              QStringLiteral("the status reads Active (%1)").arg(text));
    }

    // The effective limits must be shown, not "Uncapped".
    bool foundLimits = false;
    for (QLabel *l : win->findChildren<QLabel *>()) {
        const QString text = l->text();
        if (text.startsWith(QLatin1String("Download Limit:"))) {
            foundLimits = true;
            std::printf("  limits text: \"%s\"\n", qPrintable(text));
            check(!text.contains(QLatin1String("Uncapped")),
                  "the limits are not reported as Uncapped");
            check(text.contains(QLatin1String("MB/s"))
                      || text.contains(QLatin1String("KB/s")),
                  QStringLiteral("a real rate is shown (%1)").arg(text.section('\n', 0, 0)));
            break;
        }
    }
    check(foundLimits, "the limiter page shows the effective limits");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_limiter_page passed."
                                        : "test_limiter_page FAILED.");

    win->close();
    delete win;
    return g_failures == 0 ? 0 : 1;
}
