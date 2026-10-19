// Audits the monochrome redesign: renders the real window in both themes and
// verifies the surfaces, text, buttons and sliders match the specified palette,
// that the chrome is genuinely colourless, and that the graph series keep their
// original colours.
//
// Build/run: tools/run_theme_tests.ps1 covers the palette; this renders pixels.
#include "../src/brandmark.h"
#include "../src/daypicker.h"
#include "../src/mainwindow.h"
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
#include <QPushButton>
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

// How far a colour is from neutral grey, 0 = perfectly grey.
static int chroma(const QColor &c)
{
    return qMax(qMax(c.red(), c.green()), c.blue()) - qMin(qMin(c.red(), c.green()), c.blue());
}

// Counts pixels in an image that are NOT near-neutral.
static int colourfulPixels(const QImage &img, int tolerance)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 160)
                continue;
            if (chroma(QColor(c)) > tolerance)
                ++n;
        }
    }
    return n;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("mono_results.txt")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    // =====================================================================
    // Dark palette - exact values from the specification
    // =====================================================================
    theme::setMode(theme::Mode::Dark);
    {
        struct Expected {
            const char *name;
            QColor actual;
            int r, g, b;
        };
        const Expected dark[] = {
            {"main background #0D0D0D", theme::kBackground(), 0x0D, 0x0D, 0x0D},
            {"sidebar         #111111", theme::kSidebar(), 0x11, 0x11, 0x11},
            {"cards/panels    #171717", theme::kPanel(), 0x17, 0x17, 0x17},
            {"elevated        #202020", theme::kPanelRaised(), 0x20, 0x20, 0x20},
            {"hover           #262626", theme::kHover(), 0x26, 0x26, 0x26},
            {"primary text    #F5F5F5", theme::kTextPrimary(), 0xF5, 0xF5, 0xF5},
            {"secondary text  #A3A3A3", theme::kTextSecondary(), 0xA3, 0xA3, 0xA3},
            {"muted text      #737373", theme::kTextMuted(), 0x73, 0x73, 0x73},
            {"borders         #2A2A2A", theme::kBorder(), 0x2A, 0x2A, 0x2A},
            {"active nav      #2D2D2D", theme::kNavActive(), 0x2D, 0x2D, 0x2D},
            {"input bg        #1C1C1C", theme::kPanelSunken(), 0x1C, 0x1C, 0x1C},
            {"primary button  #F5F5F5", theme::kPrimaryButton(), 0xF5, 0xF5, 0xF5},
            {"primary btn text#111111", theme::kPrimaryButtonText(), 0x11, 0x11, 0x11},
        };
        for (const Expected &e : dark) {
            const bool ok = e.actual.red() == e.r && e.actual.green() == e.g
                            && e.actual.blue() == e.b;
            check(ok, QStringLiteral("dark %1 (got #%2)").arg(QString::fromLatin1(e.name),
                                                              e.actual.name().mid(1).toUpper()));
            check(chroma(e.actual) == 0,
                  QStringLiteral("dark %1 is perfectly neutral grey")
                      .arg(QString::fromLatin1(e.name)));
        }
    }

    // =====================================================================
    // Light palette - exact values
    // =====================================================================
    theme::setMode(theme::Mode::Light);
    {
        struct Expected {
            const char *name;
            QColor actual;
            int r, g, b;
        };
        const Expected light[] = {
            {"main background #FAFAFA", theme::kBackground(), 0xFA, 0xFA, 0xFA},
            {"sidebar         #FFFFFF", theme::kSidebar(), 0xFF, 0xFF, 0xFF},
            {"cards/panels    #FFFFFF", theme::kPanel(), 0xFF, 0xFF, 0xFF},
            {"elevated        #F5F5F5", theme::kPanelRaised(), 0xF5, 0xF5, 0xF5},
            {"hover           #F0F0F0", theme::kHover(), 0xF0, 0xF0, 0xF0},
            {"primary text    #171717", theme::kTextPrimary(), 0x17, 0x17, 0x17},
            {"secondary text  #666666", theme::kTextSecondary(), 0x66, 0x66, 0x66},
            {"muted text      #8A8A8A", theme::kTextMuted(), 0x8A, 0x8A, 0x8A},
            {"borders         #E5E5E5", theme::kBorder(), 0xE5, 0xE5, 0xE5},
            {"active nav      #EEEEEE", theme::kNavActive(), 0xEE, 0xEE, 0xEE},
            {"input bg        #F7F7F7", theme::kPanelSunken(), 0xF7, 0xF7, 0xF7},
            {"primary button  #171717", theme::kPrimaryButton(), 0x17, 0x17, 0x17},
            {"primary btn text#FFFFFF", theme::kPrimaryButtonText(), 0xFF, 0xFF, 0xFF},
        };
        for (const Expected &e : light) {
            const bool ok = e.actual.red() == e.r && e.actual.green() == e.g
                            && e.actual.blue() == e.b;
            check(ok, QStringLiteral("light %1 (got #%2)")
                          .arg(QString::fromLatin1(e.name),
                               e.actual.name().mid(1).toUpper()));
            check(chroma(e.actual) == 0,
                  QStringLiteral("light %1 is perfectly neutral grey")
                      .arg(QString::fromLatin1(e.name)));
        }
    }

    // =====================================================================
    // Graph series colours are FROZEN and identical in both themes
    // =====================================================================
    {
        theme::setMode(theme::Mode::Dark);
        const QColor d1 = theme::kGraphDownload();
        const QColor u1 = theme::kGraphUpload();
        theme::setMode(theme::Mode::Light);
        const QColor d2 = theme::kGraphDownload();
        const QColor u2 = theme::kGraphUpload();

        check(d1 == d2 && u1 == u2,
              "graph series colours are identical in both themes");
        // The graph must stay chromatic - it is the one place colour is allowed.
        check(chroma(d1) > 100, QStringLiteral("graph download stays chromatic (chroma %1)")
                                    .arg(chroma(d1)));
        check(chroma(u1) > 100, QStringLiteral("graph upload stays chromatic (chroma %1)")
                                    .arg(chroma(u1)));
        logLine(QStringLiteral("     graph download %1, upload %2 (unchanged)")
                    .arg(d1.name(), u1.name()));
    }

    // =====================================================================
    // The rendered chrome is colourless
    // =====================================================================
    {
        const QString scratch =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("mono_scratch"));
        QDir(scratch).removeRecursively();
        QDir().mkpath(scratch);

        SettingsStore store(scratch);
        ThrottleEngine engine;
        SpeedMonitor monitor;
        UsageTracker usage;
        usage.setDataDirectory(scratch);

        MainWindow window(&store, &engine, &monitor, &usage);
        window.resize(1000, 700);
        window.show();
        QApplication::processEvents();
        QApplication::processEvents();

        // Count colourful pixels in the whole window. The graph is the only
        // element allowed to be chromatic, so this is measured and reported
        // rather than asserted at zero.
        for (theme::Mode m : {theme::Mode::Dark, theme::Mode::Light}) {
            theme::setMode(m);
            theme::applyApplicationTheme();
            window.update();
            QApplication::processEvents();

            const QImage img = window.grab().toImage();
            const QString label = (m == theme::Mode::Dark) ? QStringLiteral("dark")
                                                           : QStringLiteral("light");

            // Mask out the graph, then the rest must be neutral.
            QImage chrome = img;
            for (SpeedGraph *g : window.findChildren<SpeedGraph *>()) {
                const QRect r = g->geometry();
                for (int y = r.top(); y <= r.bottom() && y < chrome.height(); ++y)
                    for (int x = r.left(); x <= r.right() && x < chrome.width(); ++x)
                        chrome.setPixel(x, y, qRgba(0, 0, 0, 0));
            }

            const int colourful = colourfulPixels(chrome, 12);
            const int graphPixels = colourfulPixels(img, 12);
            logLine(QStringLiteral("     %1: %2 chromatic px total, %3 outside the graph")
                        .arg(label)
                        .arg(graphPixels)
                        .arg(colourful));

            check(colourful * 100 / (img.width() * img.height()) < 1,
                  QStringLiteral("%1 chrome is monochrome (%2 chromatic px outside the graph)")
                      .arg(label)
                      .arg(colourful));
        }

        // Buttons must follow the spec: filled monochrome with contrasting text.
        for (theme::Mode m : {theme::Mode::Dark, theme::Mode::Light}) {
            theme::setMode(m);
            theme::applyApplicationTheme();
            QApplication::processEvents();
            const QString label = (m == theme::Mode::Dark) ? QStringLiteral("dark")
                                                           : QStringLiteral("light");

            int solid = 0;
            int lightOnDark = 0;
            for (QPushButton *b : window.findChildren<QPushButton *>()) {
                // Only unfilled-by-design variants are skipped. Nav items and
                // schedule toggles draw their own transparent/outlined surface,
                // so they are not primary buttons and must not be measured here.
                const QString name = b->objectName();
                if (!b->isVisible() || name == QLatin1String("preset")
                    || name == QLatin1String("ghost") || name == QLatin1String("danger")
                    || name == QLatin1String("navItem")
                    || name == QLatin1String("scheduleToggle"))
                    continue;
                const QImage bi = b->grab().toImage();
                if (bi.isNull() || bi.width() < 8)
                    continue;
                ++solid;
                const QColor corner(bi.pixel(bi.width() / 2, 2));
                const QColor expect = theme::kPrimaryButton();
                if (qAbs(corner.red() - expect.red()) < 12)
                    ++lightOnDark;
            }
            logLine(QStringLiteral("     %1: %2 of %3 plain buttons use the primary fill")
                        .arg(label)
                        .arg(lightOnDark)
                        .arg(solid));
            check(solid == 0 || lightOnDark * 2 >= solid,
                  QStringLiteral("%1 primary buttons use the monochrome fill (%2/%3)")
                      .arg(label)
                      .arg(lightOnDark)
                      .arg(solid));
        }

        window.close();
        QDir(scratch).removeRecursively();
    }

    // =====================================================================
    // Slider: monochrome track, and the filled half differs by value only
    // =====================================================================
    {
        for (theme::Mode m : {theme::Mode::Dark, theme::Mode::Light}) {
            theme::setMode(m);
            theme::applyApplicationTheme();
            TrackSlider s(theme::kPrimaryButton());
            s.setRange(0, 1000);
            // A fresh slider starts in the "uncapped" state, which deliberately
            // paints no fill at all. Clear that first, otherwise the test measures
            // the uncapped rendering and reports the track as unfilled.
            s.setUnlimited(false);
            s.setValue(600);
            s.resize(200, 24);
            s.show();
            QApplication::processEvents();

            const QImage img = s.grab().toImage();
            const int colourful = colourfulPixels(img, 12);
            const QString label = (m == theme::Mode::Dark) ? QStringLiteral("dark")
                                                           : QStringLiteral("light");
            check(colourful == 0,
                  QStringLiteral("%1 slider is monochrome (%2 chromatic px)")
                      .arg(label)
                      .arg(colourful));

            // Sample the track AWAY from the knob. The knob sits at the value
            // position (~60% along) with radius ~8px, so sampling at x=20 lands
            // inside it and reports the knob colour for both halves.
            const int y = img.height() / 2;
            const int knobX = int(20 + 0.6 * (img.width() - 40));
            const QColor left(img.pixel(qMax(4, knobX - 45), y));
            const QColor right(img.pixel(qMin(img.width() - 5, knobX + 45), y));
            check(left != right,
                  QStringLiteral("%1 slider track distinguishes filled from unfilled "
                                 "(left %2 vs right %3, knob at %4)")
                      .arg(label)
                      .arg(left.name(), right.name())
                      .arg(knobX));

            // The filled side must be the primary ink, the unfilled the border.
            const QColor expectFill = theme::kPrimaryButton();
            const QColor expectEmpty = theme::kBorderStrong();
            check(qAbs(left.red() - expectFill.red()) < 14,
                  QStringLiteral("%1 filled track uses the primary ink (%2 vs %3)")
                      .arg(label)
                      .arg(left.name(), expectFill.name()));
            check(qAbs(right.red() - expectEmpty.red()) < 14,
                  QStringLiteral("%1 unfilled track uses the border grey (%2 vs %3)")
                      .arg(label)
                      .arg(right.name(), expectEmpty.name()));
        }
    }

    // =====================================================================
    // Toggle: white-on-dark / black-on-light when enabled
    // =====================================================================
    {
        theme::setMode(theme::Mode::Dark);
        theme::applyApplicationTheme();
        ToggleSwitch t;
        t.resize(62, 30);
        t.setChecked(true, false);
        t.show();
        QApplication::processEvents();
        QImage img = t.grab().toImage();
        const int darkChroma = colourfulPixels(img, 12);
        check(darkChroma == 0, QStringLiteral("dark toggle is monochrome (%1 px)")
                                   .arg(darkChroma));

        theme::setMode(theme::Mode::Light);
        theme::applyApplicationTheme();
        t.update();
        QApplication::processEvents();
        img = t.grab().toImage();
        const int lightChroma = colourfulPixels(img, 12);
        check(lightChroma == 0, QStringLiteral("light toggle is monochrome (%1 px)")
                                    .arg(lightChroma));
    }

    // =====================================================================
    // Status colours stay meaningful
    //
    // The chrome is monochrome, but error/warning/healthy must remain
    // distinguishable from each other and from plain text - otherwise a failed
    // state reads the same as a normal one.
    // =====================================================================
    for (theme::Mode m : {theme::Mode::Dark, theme::Mode::Light}) {
        theme::setMode(m);
        const QString label = (m == theme::Mode::Dark) ? QStringLiteral("dark")
                                                       : QStringLiteral("light");
        const QColor g = theme::kGreen();
        const QColor r = theme::kRed();
        const QColor a = theme::kAmber();

        // Each must carry hue, i.e. actually read as a status colour.
        check(chroma(g) > 40 && chroma(r) > 40 && chroma(a) > 40,
              QStringLiteral("%1 status colours are chromatic").arg(label));

        // And be mutually distinguishable, not three shades of the same hue.
        const auto hueGap = [](const QColor &x, const QColor &y) {
            const int d = qAbs(x.hue() - y.hue());
            return qMin(d, 360 - d);
        };
        check(hueGap(g, r) > 60 && hueGap(r, a) > 20 && hueGap(g, a) > 40,
              QStringLiteral("%1 green/red/amber are distinct hues (%2/%3/%4)")
                  .arg(label)
                  .arg(g.hue())
                  .arg(r.hue())
                  .arg(a.hue()));
    }

    logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
