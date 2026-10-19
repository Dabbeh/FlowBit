// Probes the App Limiter page through the real MainWindow.
//
// The enforcement path needs Administrator rights, so this checks the things that
// can be checked without them: that the page exists at the right stack index, that
// cards appear only for user-added applications, that the "not enforcing" state is
// reported rather than hidden, and that the sidebar entry lines up with its page.
//
// Build/run: tools/run_applimit_probe.ps1
#include "../src/mainwindow.h"
#include "../src/applimiterpage.h"
#include "../src/applimitengine.h"
#include "../src/schedule.h"
#include "../src/settingsstore.h"
#include "../src/speedmonitor.h"
#include "../src/throttleengine.h"
#include "../src/usagetracker.h"
#include "../src/sidebar.h"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextStream>

#include <cstdio>

using namespace net;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString scratch = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("applimit_probe_scratch"));
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    SettingsStore store(scratch);
    ThrottleEngine engine;
    SpeedMonitor monitor;
    UsageTracker usage;
    usage.setDataDirectory(scratch);

    // Seed three applications, one of them disabled and one with a schedule, so the
    // card states are all exercised.
    Settings s = store.load();
    AppRule a;
    a.id = nextRuleId();
    a.exe = QStringLiteral("steam.exe");
    a.name = QStringLiteral("Steam");
    a.path = QStringLiteral("C:/Program Files (x86)/Steam/steam.exe");
    a.down = 200 * 1024;
    a.up = kUnlimited;
    s.appRules.append(a);

    AppRule b;
    b.id = nextRuleId();
    b.exe = QStringLiteral("chrome.exe");
    b.name = QStringLiteral("Google Chrome");
    b.down = 0;
    b.up = 100 * 1024;
    b.enabled = false;
    s.appRules.append(b);

    AppRule c;
    c.id = nextRuleId();
    c.exe = QStringLiteral("discord.exe");
    c.name = QStringLiteral("Discord");
    c.down = 500 * 1024;
    Rule night;
    night.id = nextRuleId();
    night.from = QStringLiteral("22:00");
    night.to = QStringLiteral("07:00");
    night.days = 0x7F;
    c.windows.append(night);
    s.appRules.append(c);

    store.save(s);

    MainWindow window(&store, &engine, &monitor, &usage);
    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();

    std::printf("=== App Limiter probe ===\n\n");

    // ---- the sidebar entry and its page line up --------------------------
    auto *pages = window.findChild<QStackedWidget *>();
    std::printf("stacked pages: %d (expected 6)\n", pages ? pages->count() : -1);
    if (pages) {
        for (int i = 0; i < pages->count(); ++i) {
            QWidget *w = pages->widget(i);
            std::printf("  [%d] %s  class=%s\n", i, qPrintable(w->objectName()),
                        w->metaObject()->className());
        }
    }

    auto *page = window.findChild<AppLimiterPage *>();
    std::printf("AppLimiterPage found: %s\n", page ? "yes" : "NO");
    if (pages && page) {
        // Each page is wrapped by makePage(), so the AppLimiterPage is a child of
        // the widget at its stack index rather than the stacked widget itself.
        int hostingIndex = -1;
        for (int i = 0; i < pages->count(); ++i) {
            QWidget *w = pages->widget(i);
            if (w == page || w->isAncestorOf(page))
                hostingIndex = i;
        }
        std::printf("hosted at stack index: %d (Sidebar::AppLimiter == %d)\n", hostingIndex,
                    int(Sidebar::AppLimiter));
        std::printf("index matches the sidebar enum: %s\n",
                    hostingIndex == int(Sidebar::AppLimiter) ? "yes" : "NO - MISMATCH");

        // Selecting that index must actually reveal the page.
        pages->setCurrentIndex(hostingIndex);
        QApplication::processEvents();
        std::printf("selecting it makes the page visible: %s\n",
                    page->isVisible() ? "yes" : "NO");
    }

    if (!page) {
        std::printf("\nFAILED: the page is not in the window.\n");
        return 1;
    }

    // ---- only user-added applications are listed -------------------------
    const int cards = page->cardCount();
    std::printf("\ncards rendered: %d (expected 3, one per added application)\n", cards);
    std::printf("no card for an application the user did not add: %s\n",
                cards == 3 ? "confirmed" : "CHECK FAILED");

    // Collect the visible card labels to confirm names and limit text.
    std::printf("\ncard contents:\n");
    for (QLabel *l : page->findChildren<QLabel *>()) {
        const QString t = l->text().trimmed();
        if (t.isEmpty() || !l->isVisible())
            continue;
        if (t == QLatin1String("Steam") || t == QLatin1String("Google Chrome")
            || t == QLatin1String("Discord") || t.contains(QLatin1String("KB/s"))
            || t.contains(QLatin1String("Unlimited")) || t.contains(QLatin1String("Active"))
            || t.contains(QLatin1String("Off")) || t.contains(QLatin1String("22:00"))
            || t.contains(QLatin1String("Always on"))) {
            std::printf("   %s\n", qPrintable(t));
        }
    }

    // ---- the not-enforcing state must be stated --------------------------
    std::printf("\nenforcement banner and state (this shell is not elevated):\n");
    bool saidNotEnforcing = false;
    for (QLabel *l : page->findChildren<QLabel *>()) {
        const QString t = l->text();
        if (!l->isVisible() || t.isEmpty())
            continue;
        if (t.contains(QLatin1String("not being enforced"), Qt::CaseInsensitive)
            || t.contains(QLatin1String("Administrator"), Qt::CaseInsensitive)
            || t.contains(QLatin1String("Not enforcing"), Qt::CaseInsensitive)) {
            std::printf("   %s\n", qPrintable(t));
            saidNotEnforcing = true;
        }
    }
    std::printf("the page reports that limits are not in force: %s\n",
                saidNotEnforcing ? "yes" : "NO - it would mislead the user");

    // ---- controls exist on every card ------------------------------------
    int edits = 0, schedules = 0, deletes = 0, toggles = 0;
    for (QPushButton *btn : page->findChildren<QPushButton *>()) {
        if (btn->text() == QLatin1String("Edit")) ++edits;
        if (btn->text() == QLatin1String("Schedule")) ++schedules;
        if (btn->text() == QLatin1String("Delete")) ++deletes;
        if (btn->objectName() == QLatin1String("scheduleToggle")) ++toggles;
    }
    std::printf("\ncontrols: Edit=%d Schedule=%d Delete=%d toggles=%d (expected 3 each)\n",
                edits, schedules, deletes, toggles);

    // ---- the empty state -------------------------------------------------
    {
        Settings empty = store.load();
        empty.appRules.clear();
        store.save(empty);
        page->setRules({});
        QApplication::processEvents();
        bool emptyShown = false;
        for (QLabel *l : page->findChildren<QLabel *>()) {
            if (l->isVisible() && l->text().contains(QLatin1String("No applications are limited")))
                emptyShown = true;
        }
        std::printf("\nempty state shown when no applications are added: %s\n",
                    emptyShown ? "yes" : "NO");
        std::printf("cards after clearing: %d (expected 0)\n", page->cardCount());
    }

    window.close();
    QDir(scratch).removeRecursively();
    return 0;
}
