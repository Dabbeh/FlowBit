// Reports the Settings page's layout as numbers, with no screenshot involved.
//
// Two attempts at a screenshot produced a stale image: the scrollbar moved and grab() kept
// returning the same top-of-page render. Rather than keep fighting the capture, this asks
// the widgets directly, which is unambiguous:
//
//   * is the Startup card in the page's layout?
//   * at what y does it sit, and is it inside the scrollable extent?
//   * what does each control contain?
//
// A card that is built and laid out is what matters; the screenshot was only ever a way of
// seeing that, and it can be replaced by geometry.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QVBoxLayout>
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

    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1280, 820);
    win->show();
    pump(600);

    if (auto *side = win->findChild<Sidebar *>())
        win->onPageSelected(int(Sidebar::Settings));
    pump(500);

    // Force a layout pass now that the page is current. Without it the scroll area can still
    // hold a 0px viewport, which is a property of this harness rather than of the page: a
    // real session gets the pass from the window manager.
    win->resize(1279, 819);
    pump(400);
    win->resize(1280, 820);
    pump(400);

    std::printf("\n== the Startup card, measured ==\n");

    // ---- 1. the controls exist -------------------------------------------
    // The three controls carry no text of their own for the checkboxes; they are labelled by
    // the row beside them, so they are found by walking the card.
    QWidget *card = nullptr;
    for (QLabel *l : win->findChildren<QLabel *>()) {
        if (l->text() == QLatin1String("Launch Flowbit when Windows starts")) {
            QWidget *p = l->parentWidget();
            while (p && !qobject_cast<QFrame *>(p))
                p = p->parentWidget();
            card = p;
            break;
        }
    }
    check(card != nullptr, "the Startup card is in the widget tree");

    const QList<QCheckBox *> checkboxes = card ? card->findChildren<QCheckBox *>() : QList<QCheckBox *>();
    const QList<QComboBox *> combos = card ? card->findChildren<QComboBox *>() : QList<QComboBox *>();
    const QList<QPushButton *> buttons = card ? card->findChildren<QPushButton *>() : QList<QPushButton *>();

    check(checkboxes.size() == 2,
          QStringLiteral("it holds two checkboxes, for startup and updates (%1)")
              .arg(checkboxes.size()));
    check(combos.size() == 1,
          QStringLiteral("and one combo, for what closing does (%1)").arg(combos.size()));
    check(buttons.size() == 1,
          QStringLiteral("and one button, Check now (%1)").arg(buttons.size()));

    if (!card) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    // ---- 2. it is laid out, not merely constructed ------------------------
    // This is the check that would have caught the card being placed after `return page;`:
    // constructed widgets that are never added to a layout have no size.
    check(card->height() > 100,
          QStringLiteral("the card has a real height, so it was added to the page layout (%1px)")
              .arg(card->height()));
    check(card->width() > 400,
          QStringLiteral("and a real width (%1px)").arg(card->width()));

    // ---- 3. it sits inside the scrollable extent -------------------------
    auto *area = win->findChild<QScrollArea *>();
    check(area != nullptr, "the settings page scrolls");
    // Let the scroll area lay its content out before measuring it: the height read here is
    // the CONTENT height, which is only correct once the layout has run.
    pump(300);
    if (area && area->widget()) {
        int y = 0;
        for (QWidget *w = card; w && w != area->widget(); w = w->parentWidget())
            y += w->y();

        // The scrollbar's maximum is the authoritative statement about how far the page
        // scrolls. area->widget()->height() was read before the content had been laid out
        // and reported the viewport height instead, which made a reachable card look
        // unreachable.
        QScrollBar *bar = area->verticalScrollBar();
        const int viewport = area->viewport()->height();
        const int reach = bar ? bar->maximum() + viewport : 0;
        std::printf("    card top %dpx; viewport %dpx; reaches %dpx\n", y, viewport, reach);

        check(reach >= y + card->height(),
              "the card is within the scrollable extent, so it can be reached");
        check(bar && bar->maximum() > 0,
              "and the page genuinely scrolls rather than clipping the card");
    }

    // ---- 4. the controls hold the saved settings -------------------------
    std::printf("    close combo   : \"%s\" (data %d)\n", qPrintable(combos.first()->currentText()),
                combos.first()->currentData().toInt());
    std::printf("    startup check : %d\n", checkboxes.at(0)->isChecked() ? 1 : 0);
    std::printf("    update check  : %d\n", checkboxes.at(1)->isChecked() ? 1 : 0);

    const Settings saved = store.load();
    check(combos.first()->currentData().toInt() == (saved.closeToTray ? 1 : 0),
          "the close choice reflects the saved setting");
    check(checkboxes.at(0)->isChecked() == saved.startWithWindows,
          "the startup box reflects the saved setting");
    check(checkboxes.at(1)->isChecked() == saved.checkForUpdates,
          "the update box reflects the saved setting");

    // ---- 5. the conflict guard ------------------------------------------
    // Turning the startup box on writes the registry entry, and the note under the controls
    // is what reports a refusal or a stale path. It must exist and be reachable.
    bool noteFound = false;
    for (QLabel *l : card->findChildren<QLabel *>()) {
        if (l->objectName() == QLatin1String("note"))
            noteFound = true;
    }
    check(noteFound, "the note that explains a refusal is present");

    win->close();
    delete win;

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_startup_card passed."
                                        : "test_startup_card FAILED.");
    return g_failures == 0 ? 0 : 1;
}
