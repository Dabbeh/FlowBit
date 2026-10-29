// The per-application window must be exactly the window the caption claims.
//
// The panel says "Recorded per application over the current cycle" and the unattributed remainder
// is computed against the cycle totals. It was reading a fixed 30-day window instead, so a cycle
// shorter than 30 days pulled in days from before it - row totals too large, remainder clamped to
// zero, and a real gap silently hidden.

#include "usagetracker.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

void writeAppsMonth(const QString &dir, const QDate &anyDayInMonth, const QJsonArray &days)
{
    QJsonObject root;
    root.insert(QStringLiteral("days"), days);
    QDir().mkpath(dir);
    QFile f(QStringLiteral("%1/apps-%2.json").arg(dir, anyDayInMonth.toString("yyyy-MM")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson());
}

QJsonObject appEntry(const QDate &date, const QString &path, qint64 down)
{
    QJsonObject app{{"path", path}, {"name", "Test"}, {"down", double(down)}, {"up", 0.0}};
    return QJsonObject{{"date", date.toString(Qt::ISODate)}, {"apps", QJsonArray{app}}};
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("\n== the per-application window ==\n");

    QTemporaryDir tmp;
    const QDate today = QDate::currentDate();
    const QDate old = today.addDays(-20);

    // Two months may be involved; write each entry into its own month file.
    QHash<QString, QJsonArray> byMonth;
    for (const QDate &d : {today, old}) {
        const QString month = d.toString("yyyy-MM");
        byMonth[month].append(appEntry(d, "C:/x/app.exe", (d == today) ? 100 : 900));
    }
    for (auto it = byMonth.constBegin(); it != byMonth.constEnd(); ++it) {
        const QDate monthStart = QDate::fromString(it.key() + "-01", Qt::ISODate);
        writeAppsMonth(tmp.path(), monthStart, it.value());
    }

    UsageTracker t;
    t.setDataDirectory(tmp.path());

    // The tracker loads exactly the months its cycle covers, so a window can only report days that
    // were loaded. That is correct behaviour (asking for older data would mean loading files nobody
    // asked about), and this test states it: the cycle here starts 25 days ago, so both entries are
    // inside the loaded range and the window questions are meaningful.
    UsageConfig cfg;
    cfg.cycle = CycleKind::Rolling30;
    cfg.cycleStart = today.addDays(-25);
    t.load(cfg);

    auto totalFor = [&](int days) {
        quint64 down = 0;
        for (const auto &a : t.appUsageOverDays(days))
            if (a.path == "C:/x/app.exe") down += a.down;
        return down;
    };

    const quint64 oneDay = totalFor(1);
    const quint64 twentyOneDays = totalFor(21);
    std::printf("  1-day window: %llu  21-day window: %llu\n",
                (unsigned long long)oneDay, (unsigned long long)twentyOneDays);

    check(oneDay == 100, "a one-day window counts only today");
    check(twentyOneDays == 1000, "a 21-day window includes the older entry");
    check(twentyOneDays > oneDay, "and the windows are genuinely different");

    std::printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
                g_failures == 0 ? "test_app_usage_window passed." : "FAILED.");
    return g_failures == 0 ? 0 : 1;
}
