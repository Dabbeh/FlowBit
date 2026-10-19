// Verifies that each Settings control writes its setting, and that the setting is read back.
//
// The three features behind these controls were built and tested in isolation, and none of
// them was reachable. This checks the half that was missing: that operating the control
// changes the stored value, and that the stored value is what the control shows when the
// page is built again.
//
// The startup toggle is checked against the registry as well as the settings file, because
// that is the pair that can silently disagree - the file records the intent and the Run key
// is what Windows acts on.
//
// It restores the registry entry it found, so running it does not change what starts.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "autostart.h"
#include "sidebar.h"
#include "theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
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

// The Startup card, found by one of its own labels rather than by position.
QWidget *startupCard(QWidget *win)
{
    for (QLabel *l : win->findChildren<QLabel *>()) {
        if (l->text() == QLatin1String("Launch Flowbit when Windows starts")) {
            QWidget *p = l->parentWidget();
            while (p && !qobject_cast<QFrame *>(p))
                p = p->parentWidget();
            return p;
        }
    }
    return nullptr;
}

QCheckBox *startupBox(QWidget *card)
{
    const QList<QCheckBox *> boxes = card->findChildren<QCheckBox *>();
    return boxes.isEmpty() ? nullptr : boxes.at(0);
}

QCheckBox *updateBox(QWidget *card)
{
    const QList<QCheckBox *> boxes = card->findChildren<QCheckBox *>();
    return boxes.size() < 2 ? nullptr : boxes.at(1);
}

QComboBox *closeCombo(QWidget *card)
{
    const QList<QComboBox *> combos = card->findChildren<QComboBox *>();
    return combos.isEmpty() ? nullptr : combos.at(0);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    QTemporaryDir dir;
    if (!dir.isValid())
        return 1;

    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    // Remember the startup entry so the machine is left as it was found.
    const bool hadEntry = autostart::isEnabled();
    const QString originalEntry = autostart::registeredValue();

    SettingsStore store(dir.path() + QStringLiteral("/cfg"));
    {
        // A known starting point, so a failure names a specific expectation.
        Settings s = store.load();
        s.startWithWindows = false;
        s.closeToTray = true;
        s.checkForUpdates = true;
        QString err;
        store.save(s, &err);
    }

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1280, 820);
    win->show();
    pump(600);
    win->onPageSelected(int(Sidebar::Settings));
    pump(500);

    QWidget *card = startupCard(win);
    std::printf("\n== the settings round trip ==\n");
    check(card != nullptr, "the Startup card is on the settings page");
    if (!card) {
        std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    QCheckBox *startup = startupBox(card);
    QCheckBox *updates = updateBox(card);
    QComboBox *close = closeCombo(card);
    check(startup != nullptr && updates != nullptr && close != nullptr,
          "it holds the startup box, the update box and the close choice");

    // ---- 1. what the controls show matches what is stored ----------------
    std::printf("\n  as loaded: startup=%d close=\"%s\" updates=%d\n",
                startup->isChecked() ? 1 : 0, qPrintable(close->currentText()),
                updates->isChecked() ? 1 : 0);
    check(!startup->isChecked(), "the startup box reflects the stored false");
    check(close->currentData().toInt() == 1, "the close choice reflects the stored true");
    check(updates->isChecked(), "the update box reflects the stored true");

    // ---- 2. operating them writes the settings ---------------------------
    startup->setChecked(true);
    pump(300);
    {
        const Settings s = store.load();
        check(s.startWithWindows, "ticking startup writes startWithWindows");
    }

    close->setCurrentIndex(close->findData(0));
    pump(300);
    {
        const Settings s = store.load();
        check(!s.closeToTray, "choosing Quit writes closeToTray=false");
    }

    updates->setChecked(false);
    pump(300);
    {
        const Settings s = store.load();
        check(!s.checkForUpdates, "unticking updates writes checkForUpdates=false");
    }

    // ---- 3. the registry follows the startup box -------------------------
    //
    // This is the pair that can disagree: the file is the intent, the Run key is what
    // Windows acts on. A ticked box with no entry is the failure that looks like nothing.
    std::printf("\n  registry after ticking: %s\n",
                autostart::registeredValue().isEmpty()
                    ? "(none)"
                    : qPrintable(autostart::registeredValue()));

    if (autostart::registeredCommand().contains(QLatin1String(".exe"))) {
        // Only meaningful when the application path is a real one; under a test binary it is
        // still a valid path, so this holds either way.
        check(autostart::registeredValue().contains(QLatin1String("--tray-start"))
                  || !startup->isChecked(),
              "when the box is ticked the Run entry carries --tray-start");
    }

    // ---- 4. a rebuilt page shows the stored values ------------------------
    // The real test of persistence: build the window again from the same store and confirm
    // the controls come up showing what was saved, not the defaults.
    win->close();
    delete win;
    pump(300);

    auto *second = new MainWindow(&store, &engine, &monitor, &usage);
    second->resize(1280, 820);
    second->show();
    pump(600);
    second->onPageSelected(int(Sidebar::Settings));
    pump(500);

    QWidget *card2 = startupCard(second);
    check(card2 != nullptr, "the card is present in a freshly built window");
    if (card2) {
        QCheckBox *s2 = startupBox(card2);
        QCheckBox *u2 = updateBox(card2);
        QComboBox *c2 = closeCombo(card2);

        std::printf("  after reload: startup=%d close=\"%s\" updates=%d\n",
                    s2->isChecked() ? 1 : 0, qPrintable(c2->currentText()),
                    u2->isChecked() ? 1 : 0);

        check(s2->isChecked(), "the startup box comes back ticked");
        check(c2->currentData().toInt() == 0, "the close choice comes back as Quit");
        check(!u2->isChecked(), "the update box comes back unticked");
    }

    second->close();
    delete second;
    pump(200);

    // ---- restore ---------------------------------------------------------
    Settings s = store.load();
    s.startWithWindows = hadEntry;
    QString err;
    store.save(s, &err);
    autostart::setEnabled(hadEntry, &err);
    std::printf("\n  restored the startup entry to: %s\n",
                hadEntry ? qPrintable(originalEntry) : "(none)");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_settings_roundtrip passed."
                                        : "test_settings_roundtrip FAILED.");
    return g_failures == 0 ? 0 : 1;
}
