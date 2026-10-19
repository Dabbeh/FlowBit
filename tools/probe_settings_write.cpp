// Finds out why the settings controls do not write.
//
// The round-trip test showed all three failing: ticking the box, changing the combo and
// unticking the update box each left the stored value unchanged. The handlers are present in
// the source, so the question is whether they run at all.
//
// This connects to the same signals independently and reports what arrives, and reads the
// stored settings before and after. That distinguishes three possibilities:
//
//   * the signal never fires, so the handler is not reached;
//   * the signal fires and the handler runs, but the save does not persist;
//   * the handler is reached and bails out early.
//
// It also checks the one thing the handlers all share: m_loading. Every one of them returns
// immediately when it is true, and it is only cleared at the end of the settings-load path -
// so if anything re-enters that path, the controls become write-only no-ops.

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
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

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

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    QTemporaryDir dir;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    SettingsStore store(dir.path() + QStringLiteral("/cfg"));
    {
        Settings s = store.load();
        s.startWithWindows = false;
        s.closeToTray = true;
        s.checkForUpdates = true;
        QString err;
        std::printf("  initial save ok: %d  (%s)\n", store.save(s, &err) ? 1 : 0,
                    qPrintable(err));
        std::printf("  file: %s\n", qPrintable(store.filePath()));
        std::printf("  reloaded: startup=%d close=%d updates=%d\n",
                    store.load().startWithWindows ? 1 : 0, store.load().closeToTray ? 1 : 0,
                    store.load().checkForUpdates ? 1 : 0);
    }

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1280, 820);
    win->show();
    pump(600);
    win->onPageSelected(int(Sidebar::Settings));
    pump(500);

    QWidget *card = startupCard(win);
    if (!card) {
        std::printf("  no card\n");
        return 1;
    }

    const QList<QCheckBox *> boxes = card->findChildren<QCheckBox *>();
    const QList<QComboBox *> combos = card->findChildren<QComboBox *>();
    QCheckBox *startup = boxes.at(0);
    QCheckBox *updates = boxes.size() > 1 ? boxes.at(1) : nullptr;
    QComboBox *close = combos.isEmpty() ? nullptr : combos.at(0);

    std::printf("\n== does the signal reach anything? ==\n");

    // Listen to the same signals the handlers use, independently.
    int toggles = 0;
    int combosSeen = 0;
    QObject::connect(startup, &QCheckBox::toggled, [&toggles](bool) { ++toggles; });
    QObject::connect(startup, &QCheckBox::stateChanged, [&toggles](int) { ++toggles; });
    if (close)
        QObject::connect(close, QOverload<int>::of(&QComboBox::currentIndexChanged),
                         [&combosSeen](int) { ++combosSeen; });

    std::printf("  startup box before: checked=%d\n", startup->isChecked() ? 1 : 0);
    startup->setChecked(true);
    pump(400);
    std::printf("  after setChecked(true): checked=%d  signals seen=%d\n",
                startup->isChecked() ? 1 : 0, toggles);

    Settings afterTick = store.load();
    std::printf("  stored startWithWindows = %d\n", afterTick.startWithWindows ? 1 : 0);

    if (close) {
        std::printf("\n  close combo before: \"%s\" (data %d)\n", qPrintable(close->currentText()),
                    close->currentData().toInt());
        const int quitAt = close->findData(0);
        std::printf("  index for Quit is %d\n", quitAt);
        close->setCurrentIndex(quitAt);
        pump(400);
        std::printf("  after selecting Quit: \"%s\"  signals seen=%d\n",
                    qPrintable(close->currentText()), combosSeen);
        Settings afterClose = store.load();
        std::printf("  stored closeToTray = %d\n", afterClose.closeToTray ? 1 : 0);
    }

    if (updates) {
        std::printf("\n  update box before: checked=%d\n", updates->isChecked() ? 1 : 0);
        updates->setChecked(false);
        pump(400);
        Settings afterUp = store.load();
        std::printf("  stored checkForUpdates = %d\n", afterUp.checkForUpdates ? 1 : 0);
    }

    // Is the window still in its loading state? That is the shared early-return.
    std::printf("\n== reload the settings explicitly and try again ==\n");
    if (QMetaObject::invokeMethod(win, "onPageSelected", Q_ARG(int, int(Sidebar::Settings))))
        pump(400);

    startup->setChecked(false);
    pump(400);
    Settings afterSecond = store.load();
    std::printf("  stored startWithWindows (after untick) = %d\n",
                afterSecond.startWithWindows ? 1 : 0);

    win->close();
    delete win;
    return 0;
}
