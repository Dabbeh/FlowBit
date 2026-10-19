// Verifies the master toggle's visual state actually follows its logical state.
//
// This exists because of a real bug: setChecked() animated a "knobPos" property
// that was never declared with Q_PROPERTY. QPropertyAnimation silently targets
// nothing in that case, so the knob never moved and the switch looked stuck on
// OFF no matter what the status bar said. A test that only checks isChecked()
// would have passed while the UI lied to the user - hence the knob assertions.
//
// Build/run: tools/run_tests.ps1 (test_toggle)
#include "../src/theme.h"
#include "../src/toggleswitch.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QThread>

#include <cmath>
#include <cstdio>

using namespace net;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", qPrintable(what));
    }
}

// Counts pixels brighter than a threshold, to confirm the plate value actually
// changes with state rather than just the internal flag.
//
// This replaced a cyan-pixel count. The toggle is monochrome now, so state is
// carried by VALUE (white when on, grey when off) rather than by hue - counting
// Mean luminance of the TRACK band, ignoring the knob.
//
// The knob deliberately inverts against the track so it stays visible: on the
// dark theme the off-state knob is near-white and the on-state knob is black.
// A whole-widget brightness count therefore mostly measures the knob rather
// than the state. The track is what encodes ON/OFF, so this samples the half the
// knob is NOT occupying.
static double trackLuma(QWidget &w, bool knobIsLeft)
{
    const QPixmap pm = w.grab();
    const QImage img = pm.toImage();
    if (img.isNull() || img.height() < 8)
        return -1.0;

    const int y0 = img.height() / 2 - 1;
    const int y1 = img.height() / 2 + 1;
    const int x0 = knobIsLeft ? (img.width() * 3) / 4 : 3;
    const int x1 = knobIsLeft ? img.width() - 4 : img.width() / 4;

    double sum = 0;
    int n = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 200)
                continue;
            sum += 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c);
            ++n;
        }
    }
    return n ? sum / n : -1.0;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Pin the dark theme: the toggle is monochrome, and on is the bright state
    // there. On the light theme on is black, so a brightness metric would be
    // measuring the opposite direction.
    net::theme::setMode(net::theme::Mode::Dark);
    net::theme::applyApplicationTheme();

    ToggleSwitch toggle;
    toggle.resize(62, 30);
    toggle.show();
    QApplication::processEvents();

    // -- initial state ------------------------------------------------------
    check(!toggle.isChecked(), "switch starts unchecked");
    check(std::fabs(toggle.knobPos() - 0.0) < 0.001,
          QStringLiteral("knob starts at the left (%1)").arg(toggle.knobPos()));
    // OFF: knob is at the left, so sample the right side of the track.
    const double offLuma = trackLuma(toggle, /*knobIsLeft=*/true);
    check(offLuma >= 0 && offLuma < 120,
          QStringLiteral("OFF track is a dark grey (luma %1)").arg(offLuma, 0, 'f', 1));

    // -- turning ON ---------------------------------------------------------
    int toggledSignals = 0;
    bool lastValue = false;
    QObject::connect(&toggle, &ToggleSwitch::toggled, [&](bool v) {
        ++toggledSignals;
        lastValue = v;
    });

    toggle.setChecked(true, false);   // no animation, so the state is immediate
    QApplication::processEvents();

    check(toggle.isChecked(), "switch reports checked after turning on");
    check(toggledSignals == 1 && lastValue, "toggled(true) was emitted once");
    check(std::fabs(toggle.knobPos() - 1.0) < 0.001,
          QStringLiteral("knob moved to the right (%1)").arg(toggle.knobPos()));
    // ON: knob is at the right, so sample the left side of the track.
    const double onLuma = trackLuma(toggle, /*knobIsLeft=*/false);
    check(onLuma > 200,
          QStringLiteral("ON track is near-white (luma %1)").arg(onLuma, 0, 'f', 1));
    check(onLuma > offLuma + 100,
          QStringLiteral("the track value changes with state (%1 -> %2)")
              .arg(offLuma, 0, 'f', 1)
              .arg(onLuma, 0, 'f', 1));

    // -- the animated path must move the knob too ---------------------------
    // This is the regression guard: with animation enabled the property must
    // still end up at its target. If knobPos is not a real Q_PROPERTY the
    // animation does nothing and this fails.
    toggle.setChecked(false, true);
    check(std::fabs(toggle.knobPos() - 1.0) < 0.001,
          "knob stays at target immediately after an animated toggle starts");

    // Spin the event loop so the animation actually runs to completion.
    for (int i = 0; i < 40; ++i) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    check(std::fabs(toggle.knobPos() - 0.0) < 0.05,
          QStringLiteral("animated toggle ends at the off position (%1)").arg(toggle.knobPos()));

    toggle.setChecked(true, true);
    for (int i = 0; i < 40; ++i) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    check(std::fabs(toggle.knobPos() - 1.0) < 0.05,
          QStringLiteral("animated toggle ends at the on position (%1)").arg(toggle.knobPos()));

    // -- idempotence and signal hygiene -------------------------------------
    const int before = toggledSignals;
    toggle.setChecked(true, false);
    check(toggledSignals == before, "setting the same state emits nothing");
    check(std::fabs(toggle.knobPos() - 1.0) < 0.001, "knob unchanged on a no-op set");

    // -- clicking toggles ---------------------------------------------------
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(30, 15), QPointF(30, 15),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(30, 15), QPointF(30, 15),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&toggle, &press);
    QApplication::sendEvent(&toggle, &release);
    QApplication::processEvents();
    check(!toggle.isChecked(), "clicking an ON switch turns it off");

    // -- renders without crashing in both states ----------------------------
    toggle.setChecked(true, false);
    const QPixmap on = toggle.grab();
    toggle.setChecked(false, false);
    const QPixmap off = toggle.grab();
    check(!on.isNull() && !off.isNull(), "both states render");
    check(on.toImage() != off.toImage(), "ON and OFF frames differ visually");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
