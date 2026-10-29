// Pass-2/3 fixes: physical limits on stored values, and history that stops growing forever.

#include "settingsstore.h"
#include "usagetracker.h"
#include "units.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>

#include <cstdio>

using namespace net;

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok) ++g_failures;
}
void writeJson(const QString &path, const QJsonObject &obj)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(obj).toJson());
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("\n== stored values cannot be absurd ==\n");

    QTemporaryDir tmp;
    const QString dir = tmp.path();

    // A settings file with impossible numbers: 1e300 down, -5000 up, a day mask of 256.
    {
        QJsonObject rule{{"downBps", 1e300}, {"upBps", -5000.0}, {"days", 256}};
        QJsonObject root{{"defaultDownBps", 1e300}, {"defaultUpBps", -1.0},
                         {"rules", QJsonArray{rule}}};
        writeJson(dir + "/settings.json", root);

        SettingsStore store(dir);
        const Settings s = store.load();
        std::printf("  defaultDown=%lld  defaultUp=%lld  rule.days=%d\n",
                    (long long)s.defaultDown, (long long)s.defaultUp,
                    s.rules.isEmpty() ? -1 : int(s.rules.first().days));
        check(s.defaultDown >= 0 && s.defaultDown <= RateBps(100.0 * 1000 * 1000 * 1000 / 8) + 1,
              "an out-of-range rate is clamped into the qint64-safe range");
        check(s.defaultUp == 0, "a negative rate becomes 0 (unlimited), not a wrapped value");
        check(!s.rules.isEmpty() && (s.rules.first().days == 0x7F),
              "a day mask wider than seven bits becomes all days, not zero days");
    }

    // History retention: an ancient month must disappear, a recent one must survive.
    {
        const QString adir = dir + "/tracker";
        QDir().mkpath(adir);
        const QDate today = QDate::currentDate();
        const QString oldName = QStringLiteral("apps-%1.json").arg(today.addMonths(-30).toString("yyyy-MM"));
        const QString newName = QStringLiteral("apps-%1.json").arg(today.toString("yyyy-MM"));
        QFile oldFile(adir + "/" + oldName);
        QFile newFile(adir + "/" + newName);
        for (QFile *f : {&oldFile, &newFile}) f->open(QIODevice::WriteOnly);
        oldFile.write("{\"days\":[]}"); oldFile.close();
        newFile.write("{\"days\":[]}"); newFile.close();

        UsageTracker t;
        t.setDataDirectory(adir);
        UsageConfig cfg;
        t.load(cfg);

        std::printf("  old month kept: %s   current month kept: %s\n",
                    QFile::exists(adir + "/" + oldName) ? "yes" : "no",
                    QFile::exists(adir + "/" + newName) ? "yes" : "no");
        check(!QFile::exists(adir + "/" + oldName), "a month 30 months old is pruned on load");
        check(QFile::exists(adir + "/" + newName), "the current month is kept");
    }

    std::printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
                g_failures == 0 ? "test_storage_limits passed." : "FAILED.");
    return g_failures == 0 ? 0 : 1;
}
