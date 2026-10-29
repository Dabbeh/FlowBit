#include "updatedownloaddialog.h"
#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "appmonitor.h"
#include "updater.h"
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <cstdio>

using namespace net;
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    for(const char *font : {"segoeui.ttf", "segoeuib.ttf", "consola.ttf"})
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/") + font);
    app.setApplicationVersion(QStringLiteral("1.0.2"));
    int failures = 0;
    auto check = [&](bool ok, const char *text) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", text); failures += !ok;
    };
    auto pump = [](int ms) {
        QElapsedTimer elapsed; elapsed.start();
        while(elapsed.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(2); }
    };
    check(Settings{}.checkForUpdates, "automatic update offers are enabled by default");
    for (auto mode : {theme::Mode::Dark, theme::Mode::Light}) {
        theme::setMode(mode); theme::applyApplicationTheme();
        UpdateDownloadDialog dialog(QStringLiteral("1.0.3"));
        dialog.show(); pump(20);
        auto *bar = dialog.findChild<QProgressBar *>();
        check(bar->maximum() == 0, "unknown download size shows an indeterminate progress bar");
        dialog.setProgress(256 * 1024, 1024 * 1024);
        check(bar->value() == 25, "known download size shows the correct percentage");
        pump(1100);
        dialog.setProgress(256 * 1024, 1024 * 1024);
        dialog.grab().save(QCoreApplication::applicationDirPath()
            + (mode == theme::Mode::Dark ? "/download-dark.png" : "/download-light.png"));
        dialog.setProgress(1024 * 1024, 1024 * 1024);
        bool verifying = false;
        for (auto *label : dialog.findChildren<QLabel *>())
            verifying |= label->text().contains(QStringLiteral("Verifying"));
        check(verifying, "completed download changes to verification status");
        int cancelled = 0;
        QObject::connect(&dialog, &QDialog::rejected, [&] { ++cancelled; });
        dialog.findChild<QPushButton *>()->click();
        check(cancelled == 1 && !dialog.isVisible(), "Cancel download emits cancellation and closes the dialog");
    }
    QTemporaryDir scratch;
    SettingsStore store(scratch.path());
    Settings settings;
    settings.enabled = false;
    settings.checkForUpdates = true;
    store.save(settings);
    QFile feed(scratch.filePath(QStringLiteral("version.json")));
    if (!feed.open(QIODevice::WriteOnly)) return 2;
    feed.write("{\"version\":\"1.0.3\",\"url\":\"https://example.invalid/Flowbit.exe\","
               "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}");
    feed.close();
    Updater::setVersionFileUrl(QUrl::fromLocalFile(feed.fileName()).toString());
    ThrottleEngine engine;
    SpeedMonitor speed;
    UsageTracker usage;
    usage.setDataDirectory(scratch.path());
    MainWindow window(&store, &engine, &speed, &usage);
    speed.stop();
    window.findChild<AppMonitor *>()->stop();
    auto *poll = window.findChild<QTimer *>(QStringLiteral("automaticUpdateTimer"));
    check(poll && poll->isActive() && poll->interval() == 30 * 60 * 1000,
          "the app checks periodically without pressing Check now");
    for(auto *timer : window.findChildren<QTimer *>()) timer->stop();
    int offers = 0;
    QTimer closeOffers;
    QObject::connect(&closeOffers, &QTimer::timeout, [&] {
        for(auto *widget : QApplication::topLevelWidgets()) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (!box || !box->isVisible() || box->windowTitle() != QStringLiteral("Update available")) continue;
            ++offers;
            for(auto *button : box->buttons())
                if(button->text() == QStringLiteral("Later")) { button->click(); break; }
        }
    });
    closeOffers.start(10);
    QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection);
    QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection);
    pump(250);
    check(offers == 1 && window.isVisible(), "a background check automatically shows one update offer, even from the tray");
    auto *snooze = window.findChild<QTimer *>(QStringLiteral("updateSnoozeTimer"));
    check(snooze && snooze->isActive(), "Later schedules a reminder");
    QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection); pump(80);
    check(offers == 1, "periodic checks respect the reminder delay");
    if(snooze) snooze->stop();
    settings.checkForUpdates = false; store.save(settings);
    QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection); pump(80);
    check(offers == 1, "turning off automatic updates suppresses background offers");
    settings.checkForUpdates = true;
    settings.skippedVersion = QStringLiteral("1.0.3"); store.save(settings);
    QMetaObject::invokeMethod(poll, "timeout", Qt::DirectConnection); pump(80);
    check(offers == 1, "a skipped version is not offered by periodic checks");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
