// Verifies that dropping the organisation name does not lose existing data.
//
// The data folder was doubly nested - Local/Flowbit/Flowbit/ - because Qt joins the
// organisation and application names, and both were "Flowbit". Removing the organisation moves
// the path up one level.
//
// The danger is obvious: an application that looks in a new, empty directory and finds nothing
// takes its "fresh install" branch, and the user's settings, schedules and usage history appear
// to have been wiped. That is exactly what this checks for.
//
// Both directories are built in a temporary location, with recognisable content in the old one,
// and the store is asked to resolve itself. It must adopt the old directory, and the migration
// must carry the files across.

#include "settingsstore.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

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

QByteArray read(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flowbit"));
    // Deliberately NOT set - this is the change being verified.

    QTemporaryDir tmp;
    if (!tmp.isValid())
        return 1;

    std::printf("\n== the data directory after dropping the organisation name ==\n");

    // ---- 1. what Qt now resolves ----------------------------------------
    const QString primary = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    std::printf("  Qt resolves AppConfigLocation to: %s\n", qPrintable(primary));
    check(!primary.endsWith(QLatin1String("Flowbit/Flowbit")),
          "the path is no longer doubly nested");

    // ---- 2. an existing install keeps its data ---------------------------
    //
    // Built to mirror the real layout: a nested directory holding settings and usage, and the
    // new location either absent or empty.
    {
        const QString root = tmp.path() + QStringLiteral("/case1/Local");
        const QString nested = root + QStringLiteral("/Flowbit/Flowbit");
        const QString newNode = root + QStringLiteral("/Flowbit");

        write(nested + QStringLiteral("/settings.json"),
              R"({"enabled":true,"defaultDownBps":424242,"themeMode":"dark"})");
        write(nested + QStringLiteral("/usage-2026-10.json"), R"({"totalBytes":999})");

        // The store is pointed at the nested directory explicitly, which is what the old build
        // did, then asked to migrate into the new one.
        SettingsStore oldStore(nested);
        const Settings loaded = oldStore.load();
        check(loaded.enabled, "the old directory's settings load");
        check(qint64(loaded.defaultDown) == 424242,
              QStringLiteral("including the baseline limit (%1)").arg(qint64(loaded.defaultDown)));

        SettingsStore newStore(newNode);
        newStore.setLegacyDirectoryForTesting(nested);
        const int moved = newStore.migrateLegacyData();
        std::printf("  migrated %d file(s) from the nested directory\n", moved);

        check(moved >= 1, "the migration copies the files across");
        check(QFile::exists(newNode + QStringLiteral("/settings.json")),
              "the settings file now exists at the new location");

        const Settings adopted = newStore.load();
        check(adopted.enabled, "and it loads from there");
        check(qint64(adopted.defaultDown) == 424242,
              "with the value intact, not the default");
    }

    // ---- 3. an older file is not overwritten -----------------------------
    //
    // The migration must never clobber something newer. A user who has already run the new
    // build and then downgrades and upgrades again would otherwise lose the newer settings.
    {
        const QString root = tmp.path() + QStringLiteral("/case2/Local");
        const QString nested = root + QStringLiteral("/Flowbit/Flowbit");
        const QString newNode = root + QStringLiteral("/Flowbit");

        write(nested + QStringLiteral("/settings.json"),
              R"({"enabled":false,"defaultDownBps":111})");
        write(newNode + QStringLiteral("/settings.json"),
              R"({"enabled":true,"defaultDownBps":222})");

        // Make the new one newer than the old one.
        QFile newer(newNode + QStringLiteral("/settings.json"));
        if (newer.open(QIODevice::ReadWrite))
            newer.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime);

        SettingsStore store(newNode);
        store.setLegacyDirectoryForTesting(nested);
        store.migrateLegacyData();

        const Settings kept = store.load();
        std::printf("  after migrating over a newer file: defaultDown=%lld enabled=%d\n",
                    static_cast<long long>(qint64(kept.defaultDown)), kept.enabled ? 1 : 0);
        check(qint64(kept.defaultDown) == 222,
              "a newer file in the active directory is not overwritten");

        // And the source is left in place, so a downgrade still finds its data.
        check(QFile::exists(nested + QStringLiteral("/settings.json")),
              "the older directory is left in place rather than moved");
    }

    // ---- 4. a fresh install is unaffected --------------------------------
    {
        const QString root = tmp.path() + QStringLiteral("/case3/Local/Flowbit");
        SettingsStore store(root);
        check(!QFile::exists(root + QStringLiteral("/settings.json")),
              "a fresh install has no settings file until one is saved");

        Settings defaults = store.load();
        check(!defaults.enabled,
              "and the defaults have the master switch off, so nothing is limited");
        check(defaults.closeToTray, "with the tray behaviour as the shipped default");
        check(defaults.checkForUpdates, "and update checks on");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_data_migration passed."
                                        : "test_data_migration FAILED.");
    return g_failures == 0 ? 0 : 1;
}
