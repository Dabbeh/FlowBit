// Verifies the NetValve -> Flowbit data migration.
//
// The rebrand moved the config directory, and that is the one part of a rename
// that can destroy user data. So this drives the REAL migrateLegacyData():
// existing settings.json and monthly usage history must carry across, the
// originals must survive (a downgrade still works), and newer files in the new
// location must never be clobbered.
//
// Build/run: tools/run_tests.ps1 (test_migration)
#include "../src/settingsstore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

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
    } else {
        std::printf("  ok: %s\n", qPrintable(what));
    }
}

static void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(content);
        f.close();
    }
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString root =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("migration_scratch"));
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    const QString legacy = root + QStringLiteral("/NetValve");
    const QString fresh = root + QStringLiteral("/Flowbit");
    QDir().mkpath(legacy);
    QDir().mkpath(fresh);

    // -- a realistic legacy install -----------------------------------------
    const QByteArray legacySettings =
        R"({"enabled":true,"defaultDownBps":3145728,"defaultUpBps":786432,"rules":[]})";
    const QByteArray legacyUsage =
        R"({"days":[{"date":"2026-10-14","down":123456,"up":7890}]})";
    writeFile(legacy + QStringLiteral("/settings.json"), legacySettings);
    writeFile(legacy + QStringLiteral("/usage-2026-10.json"), legacyUsage);
    writeFile(legacy + QStringLiteral("/usage-2026-09.json"), legacyUsage);

    // -- migration via the production code path -----------------------------
    SettingsStore store(fresh);
    store.setLegacyDirectoryForTesting(legacy);

    const int copied = store.migrateLegacyData();
    check(copied == 3, QStringLiteral("all three legacy files copy across (got %1)").arg(copied));

    check(readFile(fresh + QStringLiteral("/settings.json")) == legacySettings,
          "settings.json survives byte-for-byte");
    check(readFile(fresh + QStringLiteral("/usage-2026-10.json")) == legacyUsage,
          "current-month usage history survives byte-for-byte");
    check(readFile(fresh + QStringLiteral("/usage-2026-09.json")) == legacyUsage,
          "previous-month usage history survives too");

    // Originals must remain: a user who downgrades still finds their data.
    check(QFile::exists(legacy + QStringLiteral("/settings.json")),
          "legacy settings.json left in place (copy, not move)");
    check(QFile::exists(legacy + QStringLiteral("/usage-2026-10.json")),
          "legacy usage history left in place");

    // -- the migrated settings must load with their values intact -----------
    {
        SettingsStore loaded(fresh);
        const Settings s = loaded.load();
        check(s.enabled, "migrated settings load with enabled=true");
        check(s.defaultDown == 3145728,
              QStringLiteral("migrated download limit intact (got %1)").arg(s.defaultDown));
        check(s.defaultUp == 786432,
              QStringLiteral("migrated upload limit intact (got %1)").arg(s.defaultUp));
    }

    // -- migration is idempotent and never clobbers newer data -------------
    {
        const QByteArray newer =
            R"({"enabled":false,"defaultDownBps":9999,"rules":[]})";
        writeFile(fresh + QStringLiteral("/settings.json"), newer);
        // Make sure the legacy copy is unambiguously older than the new file.
        QFile legacyFile(legacy + QStringLiteral("/settings.json"));
        // The result is deliberately unused - the call exists only to touch the
        // file. Cast to void so -Wunused-result does not reject the build.
        (void)legacyFile.open(QIODevice::ReadWrite);
        legacyFile.setFileTime(QDateTime::currentDateTime().addSecs(-600),
                               QFileDevice::FileModificationTime);
        legacyFile.close();

        SettingsStore again(fresh);
        again.setLegacyDirectoryForTesting(legacy);
        const int recopied = again.migrateLegacyData();

        check(readFile(fresh + QStringLiteral("/settings.json")) == newer,
              "a newer file in the new location is not clobbered by the legacy copy");
        std::printf("  (re-run copied %d file(s); usage files were already current)\n", recopied);
    }

    // -- a corrupt legacy file must not break the store ---------------------
    {
        const QString badLegacy = root + QStringLiteral("/NetValveCorrupt");
        writeFile(badLegacy + QStringLiteral("/settings.json"), QByteArray("{ not json at all"));

        SettingsStore bad(badLegacy);
        const Settings s = bad.load();
        check(!s.enabled && s.defaultDown == 0,
              "corrupt legacy settings fall back to defaults instead of crashing");

        QString err;
        const bool saved = bad.save(s, &err);
        check(saved, QStringLiteral("store can overwrite corrupt legacy data (%1)").arg(err));
    }

    // -- no legacy directory at all: nothing to do, no crash ---------------
    {
        SettingsStore clean(root + QStringLiteral("/BrandNew"));
        check(clean.migrateLegacyData() == 0, "migration is a no-op when there is no legacy dir");
    }

    QDir(root).removeRecursively();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
