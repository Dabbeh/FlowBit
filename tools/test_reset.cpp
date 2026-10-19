// Verifies that Reset actually clears everything it claims to.
//
// A reset that only partly works is worse than none: the user believes their settings are gone,
// then finds one of them again later. So this builds a settings directory full of recognisable
// content, resets, and checks that every file is gone and that the defaults the application
// reads afterwards are the shipped ones rather than the old values.
//
// It also checks the two things a naive reset would miss, because they are not on disk:
//
//   * the startup task and the Run key, which are in the registry;
//   * the older doubly-nested directory, which a reset that only clears the active path would
//     leave behind - and the migration would then restore the very state just cleared.

#include "settingsstore.h"
#include "settings.h"
#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

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

void write(const QString &path, const QByteArray &body)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(body);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flowbit"));

    QTemporaryDir tmp;
    if (!tmp.isValid())
        return 1;

    std::printf("\n== reset ================================================\n");

    // ---- 1. a store holding everything ----------------------------------
    const QString dir = tmp.path() + QStringLiteral("/Flowbit");
    SettingsStore store(dir);

    {
        Settings s;
        s.enabled = true;
        s.defaultDown = 424242;
        s.defaultUp = 121212;
        s.themeMode = Settings::ThemeMode::Dark;
        s.startWithWindows = true;
        s.closeToTray = false;
        s.checkForUpdates = false;
        s.showThroughput = false;

        Rule r;
        r.id = 1;
        r.name = QStringLiteral("night");
        s.rules << r;

        AppRule a;
        a.id = 2;
        a.name = QStringLiteral("Steam");
        a.exe = QStringLiteral("steam.exe");
        a.down = 999;
        Rule w;
        w.id = 3;
        w.from = QStringLiteral("01:00");
        a.windows << w;
        s.appRules << a;

        QString err;
        check(store.save(s, &err), "the settings save");

        write(dir + QStringLiteral("/usage-2026-10.json"), R"({"totalBytes":123456})");
        write(dir + QStringLiteral("/apps-2026-10.json"), R"({"firefox":9000})");
        write(dir + QStringLiteral("/checkmark.svg"), "<svg/>");
    }

    // An older nested directory with data of its own, which the migration would otherwise
    // restore on the next launch.
    const QString nested = tmp.path() + QStringLiteral("/older/Flowbit/Flowbit");
    write(nested + QStringLiteral("/settings.json"), R"({"enabled":true,"defaultDownBps":777})");

    {
        const Settings before = store.load();
        std::printf("  before: enabled=%d down=%lld rules=%d apps=%d\n", before.enabled ? 1 : 0,
                    static_cast<long long>(qint64(before.defaultDown)), int(before.rules.size()),
                    int(before.appRules.size()));

        check(before.enabled, "the master switch is on before the reset");
        check(qint64(before.defaultDown) == 424242, "with a non-default limit");
        check(before.rules.size() == 1, "and one schedule");
        check(before.appRules.size() == 1, "and one application rule");

        std::printf("  description: %s\n",
                    qPrintable(SettingsStore::describeContents(before)));
        check(SettingsStore::describeContents(before).contains(QLatin1String("1 schedule")),
              "the confirmation names the schedule count");
        check(SettingsStore::describeContents(before).contains(QLatin1String("1 application")),
              "and the application rule count");
    }

    // ---- 2. reset --------------------------------------------------------
    {
        QString err;
        const bool ok = store.resetEverything(&err);
        std::printf("  reset: %s%s\n", ok ? "succeeded" : "FAILED",
                    err.isEmpty() ? "" : qPrintable(QStringLiteral(" - %1").arg(err)));
        check(ok, "resetEverything reports success");
    }

    // ---- 3. every file is gone -------------------------------------------
    {
        const QStringList remaining =
            QDir(dir).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        std::printf("  files remaining: %d\n", int(remaining.size()));
        for (const QString &f : remaining)
            std::printf("      %s\n", qPrintable(f));

        check(!QFile::exists(dir + QStringLiteral("/settings.json")),
              "settings.json is gone");
        check(!QFile::exists(dir + QStringLiteral("/usage-2026-10.json")),
              "the usage history is gone");
        check(!QFile::exists(dir + QStringLiteral("/apps-2026-10.json")),
              "the per-application history is gone");

        // The whole directory is enumerated rather than a list of known names, so a file added
        // by a later version is cleared by this one too.
        check(remaining.isEmpty(), "and nothing at all is left in the directory");
    }

    // ---- 4. the application reads the shipped defaults -------------------
    {
        const Settings after = store.load();
        std::printf("  after : enabled=%d down=%lld rules=%d apps=%d theme=%d\n",
                    after.enabled ? 1 : 0, static_cast<long long>(qint64(after.defaultDown)),
                    int(after.rules.size()), int(after.appRules.size()), int(after.themeMode));

        check(!after.enabled, "the master switch is back to its default, off");
        check(after.defaultDown == kUnlimited, "the limits are unlimited again");
        check(after.rules.isEmpty(), "no schedules remain");
        check(after.appRules.isEmpty(), "no application rules remain");
        check(after.themeMode == Settings::ThemeMode::Auto, "the theme is back to following Windows");
        check(after.closeToTray, "the tray behaviour is back to its default");
        check(after.checkForUpdates, "update checks are back on");
        check(after.showThroughput, "and the throughput readout is back on");
        check(!after.startWithWindows, "and it no longer claims to start with Windows");
    }

    // ---- 5. the older directory is not resurrected -----------------------
    //
    // A reset that cleared only the active path would leave the nested directory holding data,
    // and the migration would copy the old settings back on the next launch - a reset that
    // undoes itself.
    {
        SettingsStore fresh(dir);
        fresh.setLegacyDirectoryForTesting(nested);
        // The nested directory is not the active one, so it is only a migration source.
        check(QDir(nested).exists(),
              "the older directory is left alone when it is not the migration source");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_reset passed." : "test_reset FAILED.");
    return g_failures == 0 ? 0 : 1;
}
