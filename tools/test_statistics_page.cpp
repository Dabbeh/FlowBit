// Verifies the Statistics page is actually populated in the real window.
//
// The page previously created six labels with an em-dash ("\u2014") and never assigned
// them again, so it rendered as a row of dashes no matter how much traffic had been
// recorded. This asserts the opposite: after seeding the tracker with known usage,
// every card must show a number, and the per-application table must have rows whose
// first row is the busiest application.
//
// It drives the real MainWindow, so it also proves the page is wired to the same
// tracker the Dashboard uses rather than to something that never runs.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "theme.h"

#include <QApplication>
#include <QDate>
#include <QDir>
#include <QLabel>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>

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

quint64 gb(double n) { return quint64(n * 1024.0 * 1024 * 1024); }

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("could not create a temporary directory\n");
        return 1;
    }

    // Seed the tracker with usage that looks like the real thing: Steam heavy, a
    // browser light, spread over a few days.
    static UsageTracker tracker;
    tracker.setDataDirectory(dir.path());
    SettingsStore store;
    Settings settings = store.load();

    tracker.load(settings.usage);
    tracker.addAppSample(QStringLiteral("C:/Games/Steam/steam.exe"), QStringLiteral("Steam"),
                         gb(11.0), gb(1.5));
    tracker.addAppSample(QStringLiteral("C:/Program Files/Mozilla Firefox/firefox.exe"),
                         QStringLiteral("Firefox"), gb(0.5), gb(0.1));
    tracker.addSample(6.0 * 1024 * 1024, 1.0 * 1024 * 1024);   // totals for the cards
    tracker.flush();

    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    MainWindow win(&store, &engine, &monitor, &tracker);
    win.resize(1536, 816);
    win.show();
    QApplication::processEvents();

    // The Statistics page is refreshed whenever usage changes.
    emit tracker.usageChanged();
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QThread::msleep(10);
    }

    std::printf("\n== statistics card labels ==\n");

    // Any label still holding only an em-dash is unpopulated. The captions are the
    // card headings, and the value label sits inside the same card.
    const auto dash = QString::fromUtf8("\u2014");
    int populated = 0;
    int stillDashes = 0;
    const auto labels = win.findChildren<QLabel *>();
    for (QLabel *l : labels) {
        const QString text = l->text();
        if (text.isEmpty())
            continue;
        // Only look at value labels, which are the ones styled statValue.
        if (l->objectName() != QLatin1String("statValue"))
            continue;
        if (!text.contains(dash))
            ++populated;
        if (text.startsWith(dash))
            ++stillDashes;
    }
    std::printf("  statValue labels populated: %d, still showing a dash: %d\n",
                populated, stillDashes);
    check(populated > 0, QStringLiteral("at least one statistics card shows a value (%1)")
                             .arg(populated));
    check(stillDashes == 0,
          QStringLiteral("no statistics card is left as a bare dash (%1)").arg(stillDashes));

    std::printf("\n== per-application usage table ==\n");
    QTableWidget *table = nullptr;
    for (QTableWidget *t : win.findChildren<QTableWidget *>()) {
        if (t->columnCount() == 4
            && t->horizontalHeaderItem(0)
            && t->horizontalHeaderItem(0)->text() == QLatin1String("Application")) {
            table = t;
            break;
        }
    }
    check(table != nullptr, "the Usage by Application table exists");
    if (table) {
        std::printf("  rows: %d\n", table->rowCount());
        for (int r = 0; r < qMin(3, table->rowCount()); ++r) {
            std::printf("    %-24s total=%-12s down=%-12s up=%s\n",
                        qPrintable(table->item(r, 0) ? table->item(r, 0)->text() : QString()),
                        qPrintable(table->item(r, 1) ? table->item(r, 1)->text() : QString()),
                        qPrintable(table->item(r, 2) ? table->item(r, 2)->text() : QString()),
                        qPrintable(table->item(r, 3) ? table->item(r, 3)->text() : QString()));
        }
        check(table->rowCount() > 0, "the table lists the recorded applications");
        check(table->rowCount() == 2,
              QStringLiteral("both seeded applications appear (%1)").arg(table->rowCount()));
        const QString first = table->item(0, 0) ? table->item(0, 0)->text() : QString();
        check(first.contains(QLatin1String("Steam"), Qt::CaseInsensitive),
              QStringLiteral("the busiest application is first ('%1')").arg(first));
        const QString total = table->item(0, 1) ? table->item(0, 1)->text() : QString();
        check(total.contains(QLatin1String("GB")),
              QStringLiteral("the total is formatted in GB as expected ('%1')").arg(total));
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_statistics_page passed."
                                        : "test_statistics_page FAILED.");
    return g_failures == 0 ? 0 : 1;
}
