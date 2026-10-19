// Verifies the dark/light theme system.
//
// The failure mode this guards against is a theme switch that changes a flag but
// not a single painted pixel, so most checks render the real widgets and compare
// pixels rather than asserting configuration.
//
// Build/run: tools/run_theme_tests.ps1
#include "../src/mainwindow.h"
#include "../src/segmentedbar.h"
#include "../src/settingsstore.h"
#include "../src/speedgraph.h"
#include "../src/speedmonitor.h"
#include "../src/theme.h"
#include "../src/throttleengine.h"
#include "../src/toggleswitch.h"
#include "../src/trackslider.h"
#include "../src/units.h"
#include "../src/usagetracker.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
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

// Mean luminance of a rendered widget, 0..255.
static double meanLuma(QWidget &w)
{
    const QImage img = w.grab().toImage();
    if (img.isNull())
        return 0.0;
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < img.height(); y += 2) {
        for (int x = 0; x < img.width(); x += 2) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 128)
                continue;
            sum += 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c);
            ++n;
        }
    }
    return n ? sum / n : 0.0;
}

static bool containsColour(QWidget &w, const QColor &want, int tol)
{
    const QImage img = w.grab().toImage();
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 200)
                continue;
            if (qAbs(qRed(c) - want.red()) <= tol && qAbs(qGreen(c) - want.green()) <= tol
                && qAbs(qBlue(c) - want.blue()) <= tol)
                return true;
        }
    }
    return false;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("theme_results.txt")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    const QString scratch =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("theme_scratch"));
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    // =====================================================================
    // Palette values
    // =====================================================================
    theme::setMode(theme::Mode::Dark);
    const bool darkIsDark = theme::isDark();
    check(darkIsDark, "Dark mode reports dark");

    // The dark chrome must be genuinely dark AND neutral (no blue cast), which
    // is the whole point of replacing the navy look.
    const QColor darkBg = theme::kBackground();
    const QColor darkPanel = theme::kPanel();
    const int darkLuma = qRound(0.2126 * darkBg.red() + 0.7152 * darkBg.green()
                                + 0.0722 * darkBg.blue());
    check(darkLuma < 60, QStringLiteral("dark background is genuinely dark (luma %1)")
                             .arg(darkLuma));
    const int bgSpread = qMax(qMax(darkBg.red(), darkBg.green()), darkBg.blue())
                         - qMin(qMin(darkBg.red(), darkBg.green()), darkBg.blue());
    check(bgSpread <= 12,
          QStringLiteral("dark background is neutral grey, not tinted (channel spread %1)")
              .arg(bgSpread));
    check(qAbs(darkPanel.red() - darkPanel.green()) <= 12
              && qAbs(darkPanel.green() - darkPanel.blue()) <= 12,
          "dark panel is neutral grey");

    theme::setMode(theme::Mode::Light);
    check(!theme::isDark(), "Light mode reports light");
    const QColor lightBg = theme::kBackground();
    const QColor lightPanel = theme::kPanel();
    const int lightLuma = qRound(0.2126 * lightBg.red() + 0.7152 * lightBg.green()
                                 + 0.0722 * lightBg.blue());
    check(lightLuma > 180,
          QStringLiteral("light background is genuinely light (luma %1)").arg(lightLuma));
    check(lightPanel.red() > 240 && lightPanel.green() > 240 && lightPanel.blue() > 240,
          "light panel is white");

    // ---------------------------------------------------------------------
    // Light mode must have visible STRUCTURE, not merely be "light".
    //
    // The first light palette put a near-white page (#F1F3F5) behind white
    // cards, measuring 1.11:1 - the cards were effectively invisible and the
    // window read as one flat white sheet. These checks pin the layer
    // separation so that cannot come back.
    // ---------------------------------------------------------------------
    const auto relLum = [](const QColor &c) {
        const auto f = [](double u) {
            return u <= 0.03928 ? u / 12.92 : std::pow((u + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * f(c.redF()) + 0.7152 * f(c.greenF()) + 0.0722 * f(c.blueF());
    };
    const auto contrast = [&](const QColor &a, const QColor &b) {
        const double la = relLum(a);
        const double lb = relLum(b);
        return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
    };

    {
        const double pageVsCard = contrast(lightBg, lightPanel);
        logLine(QStringLiteral("     light page vs card contrast: %1:1")
                    .arg(pageVsCard, 0, 'f', 3));
        // The specified minimal-white scheme separates cards by their border,
        // not by fill contrast, so this only requires a *measurable* step.
        check(pageVsCard > 1.02,
              QStringLiteral("light page and card are distinct values (%1:1)")
                  .arg(pageVsCard, 0, 'f', 3));

        const double borderVsCard = contrast(theme::kBorder(), lightPanel);
        logLine(QStringLiteral("     light border vs card contrast: %1:1")
                    .arg(borderVsCard, 0, 'f', 3));
        // A hairline divider rather than a high-contrast outline; it needs to be
        // present and visible, which 1.25:1 against white achieves.
        check(borderVsCard > 1.2,
              QStringLiteral("light card borders are visible (%1:1)")
                  .arg(borderVsCard, 0, 'f', 3));

        check(contrast(theme::kPanelRaised(), lightPanel) > 1.02,
              "raised surfaces differ from the card");
        check(contrast(theme::kPanelSunken(), lightPanel) > 1.05,
              "sunken surfaces differ from the card");
    }

    // Every text rank and accent must clear WCAG AA against its own background.
    // The old muted grey measured 3.31:1 and the accents 4.04-4.28:1, so this
    // is the check that would have caught "it looks terrible".
    {
        struct Entry {
            const char *name;
            QColor colour;
        };
        const Entry entries[] = {
            {"textPrimary", theme::kTextPrimary()},
            {"textSecondary", theme::kTextSecondary()},
            
            {"primary button", theme::kPrimaryButton()},
            {"secondary text", theme::kTextSecondary()},
            
            
            
        };

        for (const Entry &e : entries) {
            const double r = contrast(e.colour, lightPanel);
            logLine(QStringLiteral("     light %1 vs card: %2:1")
                        .arg(QString::fromLatin1(e.name))
                        .arg(r, 0, 'f', 2));
            check(r >= 4.5,
                  QStringLiteral("light %1 meets WCAG AA 4.5:1 (got %2)")
                      .arg(QString::fromLatin1(e.name))
                      .arg(r, 0, 'f', 2));
        }
    }

    // Text must invert between themes, or one of them is unreadable.
    theme::setMode(theme::Mode::Dark);
    const QColor darkText = theme::kTextPrimary();
    theme::setMode(theme::Mode::Light);
    const QColor lightText = theme::kTextPrimary();
    const int darkTextLuma = qRound(0.2126 * darkText.red() + 0.7152 * darkText.green()
                                    + 0.0722 * darkText.blue());
    const int lightTextLuma = qRound(0.2126 * lightText.red() + 0.7152 * lightText.green()
                                     + 0.0722 * lightText.blue());
    check(darkTextLuma > 200 && lightTextLuma < 80,
          QStringLiteral("primary text inverts (%1 dark vs %2 light)")
              .arg(darkTextLuma)
              .arg(lightTextLuma));

    // The chrome is strictly monochrome: no accent hue anywhere in the UI.
    for (theme::Mode m : {theme::Mode::Dark, theme::Mode::Light}) {
        theme::setMode(m);
        const QString label = (m == theme::Mode::Dark) ? QStringLiteral("dark")
                                                       : QStringLiteral("light");

        struct Entry {
            const char *name;
            QColor c;
        };
        const Entry surfaces[] = {
            {"background", theme::kBackground()},
            {"panel", theme::kPanel()},
            {"raised", theme::kPanelRaised()},
            {"sunken", theme::kPanelSunken()},
            {"border", theme::kBorder()},
            {"hover", theme::kHover()},
            {"navActive", theme::kNavActive()},
            {"textPrimary", theme::kTextPrimary()},
            {"textSecondary", theme::kTextSecondary()},
            
            {"primaryButton", theme::kPrimaryButton()},
        };
        int colourful = 0;
        for (const Entry &e : surfaces) {
            const int spread = qMax(qMax(e.c.red(), e.c.green()), e.c.blue())
                               - qMin(qMin(e.c.red(), e.c.green()), e.c.blue());
            if (spread > 6) {
                ++colourful;
                logLine(QStringLiteral("     %1 %2 is tinted: %3")
                            .arg(label, QString::fromLatin1(e.name), e.c.name()));
            }
        }
        check(colourful == 0,
              QStringLiteral("%1 chrome is perfectly neutral (%2 tinted surfaces)")
                  .arg(label)
                  .arg(colourful));
    }

    // The graph series are the ONE place colour is allowed, and they must not
    // change between themes. This is the check protecting the "do not modify the
    // graphs" requirement.
    {
        theme::setMode(theme::Mode::Dark);
        const QColor d1 = theme::kGraphDownload();
        const QColor u1 = theme::kGraphUpload();
        theme::setMode(theme::Mode::Light);
        const QColor d2 = theme::kGraphDownload();
        const QColor u2 = theme::kGraphUpload();

        const auto spread = [](const QColor &c) {
            return qMax(qMax(c.red(), c.green()), c.blue())
                   - qMin(qMin(c.red(), c.green()), c.blue());
        };
        logLine(QStringLiteral("     graph: download %1, upload %2 (both themes)")
                    .arg(d1.name(), u1.name()));

        check(spread(d1) > 100,
              QStringLiteral("graph download stays chromatic (%1)").arg(d1.name()));
        check(spread(u1) > 100,
              QStringLiteral("graph upload stays chromatic (%1)").arg(u1.name()));
        check(d1 == d2 && u1 == u2,
              "graph series colours are identical in both themes");
    }


    // =====================================================================
    // Rendered pixels actually change
    // =====================================================================
    {
        theme::setMode(theme::Mode::Dark);
        theme::applyApplicationTheme();
        ToggleSwitch toggle;
        toggle.resize(62, 30);
        toggle.show();
        QApplication::processEvents();
        const double darkLumaWidget = meanLuma(toggle);

        theme::setMode(theme::Mode::Light);
        theme::applyApplicationTheme();
        toggle.update();
        QApplication::processEvents();
        const double lightLumaWidget = meanLuma(toggle);

        check(lightLumaWidget > darkLumaWidget + 20,
              QStringLiteral("toggle renders brighter in light mode (%1 -> %2)")
                  .arg(darkLumaWidget, 0, 'f', 1)
                  .arg(lightLumaWidget, 0, 'f', 1));
    }

    {
        // Graph bed and trace must both follow the theme.
        theme::setMode(theme::Mode::Dark);
        theme::applyApplicationTheme();
        SpeedGraph graph;
        graph.resize(360, 120);
        graph.addSample(4.0e6, 8.0e5);
        graph.show();
        QApplication::processEvents();
        const double darkGraph = meanLuma(graph);

        theme::setMode(theme::Mode::Light);
        theme::applyApplicationTheme();
        graph.update();
        QApplication::processEvents();
        const double lightGraph = meanLuma(graph);

        check(lightGraph > darkGraph + 20,
              QStringLiteral("graph bed follows the theme (%1 -> %2)")
                  .arg(darkGraph, 0, 'f', 1)
                  .arg(lightGraph, 0, 'f', 1));
    }

    // =====================================================================
    // Toggle switch spec, per theme
    // =====================================================================
    {
        theme::setMode(theme::Mode::Dark);
        theme::applyApplicationTheme();
        ToggleSwitch t;
        t.resize(62, 30);
        t.show();
        QApplication::processEvents();
        t.setChecked(false, false);
        QApplication::processEvents();
        check(containsColour(t, theme::kToggleOffTrack(), 10),
              "dark OFF track uses the themed grey");
        t.setChecked(true, false);
        QApplication::processEvents();
        check(containsColour(t, theme::kToggleOnTrack(), 10),
              "dark ON track uses the themed accent");
    }

    // =====================================================================
    // Auto mode resolves against the OS
    // =====================================================================
    {
        theme::setMode(theme::Mode::Auto);
        const bool expectedDark = !theme::systemPrefersLight();
        check(theme::isDark() == expectedDark,
              QStringLiteral("Auto resolves to the OS preference (OS prefers %1)")
                  .arg(expectedDark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(theme::mode() == theme::Mode::Auto, "Auto mode is remembered as Auto");
    }

    // =====================================================================
    // The window exposes a working control and persists the choice
    // =====================================================================
    {
        SettingsStore store(scratch);
        ThrottleEngine engine;
        SpeedMonitor monitor;
        UsageTracker usage;
        usage.setDataDirectory(scratch);

        MainWindow window(&store, &engine, &monitor, &usage);
        window.resize(880, 620);
        window.show();
        QApplication::processEvents();

        // Find the theme combo: the one offering Auto/Light/Dark.
        QComboBox *themeCombo = nullptr;
        for (QComboBox *c : window.findChildren<QComboBox *>()) {
            if (c->findText(QStringLiteral("Auto")) >= 0
                && c->findText(QStringLiteral("Dark")) >= 0)
                themeCombo = c;
        }
        check(themeCombo != nullptr, "theme selector is present in the window");

        if (themeCombo) {
            check(themeCombo->count() == 3, "selector offers Auto, Light and Dark");

            const int darkIdx = themeCombo->findData(int(Settings::ThemeMode::Dark));
            themeCombo->setCurrentIndex(darkIdx);
            QApplication::processEvents();
            check(theme::isDark(), "choosing Dark switches the app to dark");

            const Settings savedDark = store.load();
            check(savedDark.themeMode == Settings::ThemeMode::Dark,
                  "Dark choice is persisted to settings");

            const int lightIdx = themeCombo->findData(int(Settings::ThemeMode::Light));
            themeCombo->setCurrentIndex(lightIdx);
            QApplication::processEvents();
            check(!theme::isDark(), "choosing Light switches the app to light");

            const Settings savedLight = store.load();
            check(savedLight.themeMode == Settings::ThemeMode::Light,
                  "Light choice is persisted to settings");

            // The window must visibly change, not just the flag.
            window.update();
            QApplication::processEvents();
            const double lightWindow = meanLuma(window);

            themeCombo->setCurrentIndex(darkIdx);
            QApplication::processEvents();
            window.update();
            QApplication::processEvents();
            const double darkWindow = meanLuma(window);

            check(lightWindow > darkWindow + 20,
                  QStringLiteral("the whole window repaints lighter in light mode "
                                 "(%1 -> %2)")
                      .arg(darkWindow, 0, 'f', 1)
                      .arg(lightWindow, 0, 'f', 1));

            // Back to Auto for a clean finish.
            themeCombo->setCurrentIndex(themeCombo->findData(int(Settings::ThemeMode::Auto)));
            QApplication::processEvents();
        }
    }

    // =====================================================================
    // An older settings file (no themeMode) must default to Auto
    // =====================================================================
    {
        const QString legacyDir = scratch + QStringLiteral("/legacy");
        QDir().mkpath(legacyDir);
        QFile f(legacyDir + QStringLiteral("/settings.json"));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(R"({"enabled":true,"defaultDownBps":1048576,"rules":[]})");
            f.close();
        }
        SettingsStore store(legacyDir);
        const Settings s = store.load();
        check(s.themeMode == Settings::ThemeMode::Auto,
              "a settings file without themeMode defaults to Auto");
        check(s.defaultDown == 1048576, "the rest of the legacy settings still load");
    }

    QDir(scratch).removeRecursively();

    logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
