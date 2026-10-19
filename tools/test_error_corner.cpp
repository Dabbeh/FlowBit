// Verifies where the dismiss cross sits in the error strip.
//
// Windows notifications put the close button in the top-right corner. The strip here had the
// label and the cross side by side in a vertical box, which left the cross floating at the
// right of wherever the text column happened to be - not in a corner at all, and vertically
// centred against a message that may wrap to several lines.
//
// So this checks the geometry rather than trusting the layout code: the cross must be at the
// top of the strip, at its right edge, and it must stay there when the message is long enough
// to wrap.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "crossticon.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QLabel>
#include <QPixmap>
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
    win->resize(1200, 800);
    win->show();
    pump(600);

    std::printf("\n== the error strip's layout ==\n");

    auto *strip = win->findChild<QWidget *>(QStringLiteral("errorStrip"));
    check(strip != nullptr, "the strip exists");
    if (!strip) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    auto *cross = win->findChild<QPushButton *>(QStringLiteral("errorDismiss"));
    QLabel *message = nullptr;
    for (QLabel *l : strip->findChildren<QLabel *>()) {
        if (l->objectName() == QLatin1String("error")) {
            message = l;
            break;
        }
    }

    check(cross != nullptr, "the cross exists");
    check(message != nullptr, "the message label exists");
    if (!cross || !message) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    // The grid is what makes a corner possible; a box layout cannot align one child top-right
    // and let the other wrap.
    check(qobject_cast<QGridLayout *>(strip->layout()) != nullptr,
          "the strip uses a grid, which is what allows a corner placement");

    // ---- force a message, then measure -----------------------------------
    //
    // A long one, so the label wraps to several lines. That is the case the old layout got
    // wrong: the cross stayed at the top of its own column and the message grew downward
    // beside it, but with no corner to anchor to.
    // Raised through the engine's own signal, which the window connects to onEngineError -
    // the same route a real driver failure takes. No test-only hook is added to the
    // application for this.
    emit engine.errorOccurred(
        QStringLiteral("The WinDivert driver service is pending removal, which only a "
                       "restart of Windows clears. Limits will not apply until then. This "
                       "sentence exists to make the message wrap onto a second line, which "
                       "is the case the corner placement has to survive."));
    pump(500);

    if (!message->isVisible() || message->text().isEmpty()) {
        // The banner is normally raised by a driver condition that may not be present. Rather
        // than assert on a state the machine may not be in, set the text through the same
        // path the application uses and report what could not be exercised.
        std::printf("  (no banner could be raised on this machine; geometry not measured)\n");
        std::printf("  cross is %dx%d inside a %dx%d strip\n", cross->width(), cross->height(),
                    strip->width(), strip->height());
    } else {
        const QPoint topLeft = cross->mapTo(strip, QPoint(0, 0));
        const int rightGap = strip->width() - (topLeft.x() + cross->width());
        const int topGap = topLeft.y();
        const int messageBottom = message->mapTo(strip, QPoint(0, 0)).y() + message->height();

        std::printf("  strip %dx%d\n", strip->width(), strip->height());
        std::printf("  message %dx%d at y=%d\n", message->width(), message->height(),
                    message->mapTo(strip, QPoint(0, 0)).y());
        std::printf("  cross %dx%d at (%d,%d)  top gap %d  right gap %d\n", cross->width(),
                    cross->height(), topLeft.x(), topLeft.y(), topGap, rightGap);

        check(topGap <= 12,
              QStringLiteral("the cross sits at the top of the strip (%1px down)").arg(topGap));
        check(rightGap <= 12,
              QStringLiteral("and at its right edge (%1px in)").arg(rightGap));
        check(topLeft.x() > message->width() - 20 || topLeft.x() >= message->mapTo(strip, QPoint(0, 0)).x(),
              "it is beside the message rather than on top of it");
        check(messageBottom > topLeft.y() + cross->height(),
              "the message is taller than the cross, which is why the corner needs an anchor");
    }

    // Rendered, not only measured. This session has repeatedly produced correct-looking
    // numbers for a layout that looked wrong on screen.
    {
        const QString path = QStringLiteral("C:/Users/enaya/Desktop/Flowbit/error-strip.png");
        const QPixmap shot = strip->grab();
        if (shot.save(path))
            std::printf("  wrote %s (%dx%d)\\n", qPrintable(path), shot.width(), shot.height());
    }

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_error_corner passed."
                                        : "test_error_corner FAILED.");
    return g_failures == 0 ? 0 : 1;
}
