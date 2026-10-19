// Verifies that switching themes at RUNTIME repaints every surface.
//
// The bug this guards: widgets given their own setStyleSheet() baked a colour
// string in at construction. Reinstalling the application sheet does not touch
// those overrides, so switching dark -> light left dark cards, a dark scheduler
// well, dark rule rows and dark caption text on a white page.
//
// The test drives the real theme selector and compares rendered pixels before and
// after, rather than asserting that the palette changed.
//
// Build/run: tools/run_liveswitch_tests.ps1
#include "../src/daypicker.h"
#include "../src/mainwindow.h"
#include "../src/rulerow.h"
#include "../src/schedule.h"
#include "../src/settingsstore.h"
#include "../src/speedgraph.h"
#include "../src/speedmonitor.h"
#include "../src/theme.h"
#include "../src/throttleengine.h"
#include "../src/toggleswitch.h"
#include "../src/trackslider.h"
#include "../src/usagetracker.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QTextStream>

#include <cstdio>

using namespace net;

static int g_failures = 0;
static int g_checks = 0;
static QFile *g_log = nullptr;

static void logLine(const QString &s)
{
    std::printf("%s\n", qPrintable(s));
    std::fflush(stdout);
    if (g_log && g_log->isOpen()) {
        g_log->write(s.toUtf8());
        g_log->write("\n");
        g_log->flush();
    }
}

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        logLine(QStringLiteral("FAIL: ") + what);
    } else {
        logLine(QStringLiteral("  ok: ") + what);
    }
}

// Mean luminance of a widget, 0..255.
static double meanLuma(QWidget *w)
{
    if (!w || !w->isVisible())
        return -1.0;
    const QImage img = w->grab().toImage();
    if (img.isNull())
        return -1.0;
    double sum = 0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 160)
                continue;
            sum += 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c);
            ++n;
        }
    }
    return n ? sum / n : -1.0;
}

// Percentage of opaque pixels darker than `cutoff`.
static int darkPixelPct(QWidget *w, int cutoff)
{
    if (!w || !w->isVisible())
        return -1;
    const QImage img = w->grab().toImage();
    if (img.isNull())
        return -1;
    int dark = 0;
    int total = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 160)
                continue;
            ++total;
            const int l = qRound(0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c));
            if (l < cutoff)
                ++dark;
        }
    }
    return total ? dark * 100 / total : -1;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("liveswitch_results.txt")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    const QString scratch =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("switch_scratch"));
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    SettingsStore store(scratch);
    {
        // Seed a rule so the scheduler well and a real rule row are present.
        Settings s = store.load();
        Rule r;
        r.enabled = true;
        r.name = QStringLiteral("Work Hours");
        r.from = QStringLiteral("09:00");
        r.to = QStringLiteral("17:30");
        r.days = 0x1F;
        r.down = 4u * 1024 * 1024;
        r.up = 512u * 1024;
        s.rules.append(r);
        s.themeMode = Settings::ThemeMode::Dark;
        QString err;
        store.save(s, &err);
    }

    ThrottleEngine engine;
    SpeedMonitor monitor;
    UsageTracker usage;
    usage.setDataDirectory(scratch);

    theme::setMode(theme::Mode::Dark);

    MainWindow window(&store, &engine, &monitor, &usage);
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();
    QApplication::processEvents();

    // The selector is the one offering Auto / Light / Dark.
    QComboBox *themeCombo = nullptr;
    for (QComboBox *c : window.findChildren<QComboBox *>()) {
        if (c->findText(QStringLiteral("Auto")) >= 0
            && c->findText(QStringLiteral("Dark")) >= 0)
            themeCombo = c;
    }
    check(themeCombo != nullptr, "theme selector found");
    if (!themeCombo) {
        logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
        return 1;
    }

    // Collect the surfaces that previously went stale.
    QList<QFrame *> cards = window.findChildren<QFrame *>(QStringLiteral("card"));
    QList<QFrame *> ruleFrames = window.findChildren<QFrame *>(QStringLiteral("ruleRow"));
    auto *scroll = window.findChild<QScrollArea *>();

    logLine(QStringLiteral("     surfaces: %1 cards, %2 rule frames, scroll %3")
                .arg(cards.size())
                .arg(ruleFrames.size())
                .arg(scroll ? "yes" : "no"));

    // ---- record the dark state ------------------------------------------
    QList<double> darkCardLuma;
    for (QFrame *c : cards)
        darkCardLuma.append(meanLuma(c));
    const double darkScrollLuma = scroll ? meanLuma(scroll) : -1;
    QList<double> darkRuleLuma;
    for (QFrame *f : ruleFrames)
        darkRuleLuma.append(meanLuma(f));
    const double darkWindowLuma = meanLuma(&window);

    logLine(QStringLiteral("     dark: window luma %1").arg(darkWindowLuma, 0, 'f', 1));

    // ---- switch to LIGHT through the real control ------------------------
    themeCombo->setCurrentIndex(themeCombo->findData(int(Settings::ThemeMode::Light)));
    QApplication::processEvents();
    QApplication::processEvents();

    check(!theme::isDark(), "the app is in light mode after the switch");

    // ---- every surface must have gone light ------------------------------
    {
        int stillDark = 0;
        for (int i = 0; i < cards.size(); ++i) {
            const double before = darkCardLuma.at(i);
            const double now = meanLuma(cards.at(i));
            if (now < 0)
                continue;
            // A dark theme card sits near luma 23 (#171717); a light one near 255.
            if (now < 150) {
                ++stillDark;
                logLine(QStringLiteral("     card %1 still dark: luma %2 (was %3)")
                            .arg(i)
                            .arg(now, 0, 'f', 1)
                            .arg(before, 0, 'f', 1));
            }
        }
        check(stillDark == 0,
              QStringLiteral("all %1 cards turned light (%2 still dark)")
                  .arg(cards.size())
                  .arg(stillDark));
    }

    {
        const double now = scroll ? meanLuma(scroll) : -1;
        logLine(QStringLiteral("     scroll well: %1 -> %2")
                    .arg(darkScrollLuma, 0, 'f', 1)
                    .arg(now, 0, 'f', 1));
        check(now < 0 || now > 150,
              QStringLiteral("the scheduler well turned light (luma %1)").arg(now, 0, 'f', 1));
    }

    {
        int stillDark = 0;
        for (int i = 0; i < ruleFrames.size(); ++i) {
            const double now = meanLuma(ruleFrames.at(i));
            if (now >= 0 && now < 150)
                ++stillDark;
        }
        check(stillDark == 0,
              QStringLiteral("all %1 rule rows turned light (%2 still dark)")
                  .arg(ruleFrames.size())
                  .arg(stillDark));
    }

    {
        const double now = meanLuma(&window);
        logLine(QStringLiteral("     window: %1 -> %2")
                    .arg(darkWindowLuma, 0, 'f', 1)
                    .arg(now, 0, 'f', 1));
        check(now > 150,
              QStringLiteral("the whole window is light after the switch (luma %1)")
                  .arg(now, 0, 'f', 1));
    }

    // ---- caption text must not still be white-on-white -------------------
    {
        // Any label that kept a near-white colour would be invisible on white.
        int invisibleLabels = 0;
        for (QLabel *l : window.findChildren<QLabel *>()) {
            if (!l->isVisible() || l->text().isEmpty())
                continue;
            const QString ss = l->styleSheet();
            if (!ss.contains(QLatin1String("color:")))
                continue;
            // Pull the hex out of the stylesheet and check it against the page.
            const int hash = ss.indexOf(QLatin1Char('#'));
            if (hash < 0 || hash + 7 > ss.size())
                continue;
            const QColor c(ss.mid(hash, 7));
            if (!c.isValid())
                continue;
            const int luma = qRound(0.2126 * c.red() + 0.7152 * c.green() + 0.0722 * c.blue());
            if (luma > 230) {
                ++invisibleLabels;
                logLine(QStringLiteral("     label \"%1\" keeps a near-white colour %2")
                            .arg(l->text().left(18), c.name()));
            }
        }
        check(invisibleLabels == 0,
              QStringLiteral("no styled label is left near-white on the light page (%1)")
                  .arg(invisibleLabels));
    }

    // ---- and switching back must restore the dark surfaces ---------------
    themeCombo->setCurrentIndex(themeCombo->findData(int(Settings::ThemeMode::Dark)));
    QApplication::processEvents();
    QApplication::processEvents();

    check(theme::isDark(), "the app is dark again");
    {
        int stillLight = 0;
        for (QFrame *c : cards) {
            const double now = meanLuma(c);
            if (now > 150)
                ++stillLight;
        }
        check(stillLight == 0,
              QStringLiteral("switching back re-darkens every card (%1 still light)")
                  .arg(stillLight));
    }

    // ---- a System option must exist and resolve ---------------------------
    {
        const int sysIdx = themeCombo->findText(QStringLiteral("Auto"));
        check(sysIdx >= 0, "a System/Auto option is offered");
        themeCombo->setCurrentIndex(sysIdx);
        QApplication::processEvents();
        const Settings saved = store.load();
        check(saved.themeMode == Settings::ThemeMode::Auto,
              "choosing System persists as Auto");
    }

    window.close();
    QDir(scratch).removeRecursively();

    logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
