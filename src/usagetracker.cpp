#include "usagetracker.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace net {

namespace {

QString keyFor(const QDate &d)
{
    return d.toString(QStringLiteral("yyyy-MM-dd"));
}

// JSON has no 64-bit integer type: QJsonValue stores a double, which is exact
// for integers up to 2^53 (8 PiB). Daily totals are far below that, so storing
// them as numbers round-trips exactly - but clamp defensively so a corrupt or
// hostile file cannot produce a nonsense total.
constexpr double kMaxExact = 9007199254740992.0;  // 2^53

quint64 toBytes(const QJsonValue &v)
{
    const double d = v.toDouble(0);
    if (d <= 0)
        return 0;
    if (d >= kMaxExact)
        return static_cast<quint64>(kMaxExact);
    return static_cast<quint64>(d);
}

} // namespace

UsageTracker::UsageTracker(QObject *parent) : QObject(parent)
{
    m_dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (m_dir.isEmpty())
        m_dir = QDir::homePath() + QStringLiteral("/.Flowbit");
    m_today = QDate::currentDate();
}

void UsageTracker::setDataDirectory(const QString &dir)
{
    m_dir = dir;
}

QString UsageTracker::fileForApps(const QDate &month) const
{
    // A separate file from the day totals: the per-app table is much larger (one
    // entry per application per day), and keeping it apart means a problem reading
    // one cannot lose the other - the usage the quota depends on stays intact.
    return QStringLiteral("%1/apps-%2.json")
        .arg(m_dir, month.toString(QStringLiteral("yyyy-MM")));
}

void UsageTracker::loadAppsMonth(const QDate &month)
{
    QFile file(fileForApps(month));
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("Flowbit: ignoring corrupt per-app usage file %s (%s)",
                 qPrintable(file.fileName()), qPrintable(err.errorString()));
        return;
    }

    const QString prefix = month.toString(QStringLiteral("yyyy-MM"));
    const QJsonArray days = doc.object().value(QStringLiteral("days")).toArray();
    for (const QJsonValue &dv : days) {
        const QJsonObject dayObj = dv.toObject();
        const QDate date =
            QDate::fromString(dayObj.value(QStringLiteral("date")).toString(), Qt::ISODate);
        if (!date.isValid() || !date.toString(QStringLiteral("yyyy-MM")).startsWith(prefix))
            continue;

        // Only fill a day that has nothing recorded yet: the in-memory map holds
        // today's live accumulation, and overwriting it with what is on disk would
        // discard the seconds since the last flush.
        const QString key = date.toString(Qt::ISODate);
        if (m_appDays.contains(key))
            continue;

        QHash<QString, AppUsage> &bucket = m_appDays[key];
        const QJsonArray apps = dayObj.value(QStringLiteral("apps")).toArray();
        for (const QJsonValue &av : apps) {
            const QJsonObject o = av.toObject();
            const QString path = o.value(QStringLiteral("path")).toString();
            if (path.isEmpty())
                continue;
            AppUsage u;
            u.path = path;
            u.name = o.value(QStringLiteral("name")).toString();
            // Sanity filter on load, because a corrupted value that is already on disk would
            // otherwise be displayed forever: the accumulation bug charged a whole cumulative
            // total every second, which produced entries near 2^64 (one reached 8.6e18 bytes on a
            // real machine). A single day cannot hold a petabyte for one application on any
            // consumer machine, so anything at or above that is discarded rather than shown.
            constexpr double kMaxPlausibleDay = 1.0e15;   // 1 PB
            const double storedDown = o.value(QStringLiteral("down")).toDouble();
            const double storedUp = o.value(QStringLiteral("up")).toDouble();
            if (storedDown < 0 || storedUp < 0
                || storedDown >= kMaxPlausibleDay || storedUp >= kMaxPlausibleDay)
                continue;

            u.down = quint64(storedDown);
            u.up = quint64(storedUp);
            bucket.insert(path, u);
        }
    }
}

void UsageTracker::storeAppsMonth(const QDate &month)
{
    QDir().mkpath(m_dir);

    const QString prefix = month.toString(QStringLiteral("yyyy-MM"));
    QJsonArray days;
    for (auto it = m_appDays.constBegin(); it != m_appDays.constEnd(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        QJsonObject dayObj;
        dayObj.insert(QStringLiteral("date"), it.key());

        QJsonArray apps;
        for (const AppUsage &u : it.value()) {
            if (u.total() == 0)
                continue;   // an application that transferred nothing is noise
            QJsonObject o;
            o.insert(QStringLiteral("path"), u.path);
            o.insert(QStringLiteral("name"), u.name);
            // Doubles because JSON has no integer type; the values stay exact well
            // beyond any realistic transfer (2^53 bytes is 9 petabytes).
            o.insert(QStringLiteral("down"), double(u.down));
            o.insert(QStringLiteral("up"), double(u.up));
            apps.append(o);
        }
        if (apps.isEmpty())
            continue;
        dayObj.insert(QStringLiteral("apps"), apps);
        days.append(dayObj);
    }

    QJsonObject root;
    root.insert(QStringLiteral("days"), days);
    root.insert(QStringLiteral("savedAt"),
                QDateTime::currentDateTime().toString(Qt::ISODate));

    QSaveFile file(fileForApps(month));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("Flowbit: could not write per-app usage %s", qPrintable(file.fileName()));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit())
        qWarning("Flowbit: could not commit per-app usage %s", qPrintable(file.fileName()));
}

void UsageTracker::addAppSample(const QString &path, const QString &name,
                                quint64 downBytes, quint64 upBytes)
{
    if (path.isEmpty() || (downBytes == 0 && upBytes == 0))
        return;

    // Roll the date if the app has been running past midnight, so a sample cannot
    // be charged to the previous day.
    const QDate today = QDate::currentDate();
    if (today != m_today) {
        flush();
        m_today = today;
        m_liveDown = 0;
        m_liveUp = 0;
    }

    const QString key = m_today.toString(Qt::ISODate);
    AppUsage &u = m_appDays[key][path];
    if (u.path.isEmpty())
        u.path = path;
    if (!name.isEmpty())
        u.name = name;      // refresh the display name as it is seen
    // downBytes/upBytes are CUMULATIVE counters for this executable, not a delta.
    //
    // The previous code added them straight onto the running total, so every tick charged the
    // whole history again. On a real machine that drove one entry to 8.6e18 bytes - just under
    // 2^64, where the arithmetic wraps - and the Statistics page showed ~139 TB for a service
    // host. Steam's row looked almost normal only because its cumulative total is small.
    const QPair<quint64, quint64> previous = m_lastAppBytes.value(path, qMakePair<quint64, quint64>(0, 0));
    quint64 dDown = 0;
    quint64 dUp = 0;

    if (downBytes >= previous.first)
        dDown = downBytes - previous.first;
    else
        dDown = downBytes;      // counters restarted (monitor restarted): count what has accumulated

    if (upBytes >= previous.second)
        dUp = upBytes - previous.second;
    else
        dUp = upBytes;

    m_lastAppBytes.insert(path, qMakePair(downBytes, upBytes));

    // A single sample cannot legitimately exceed this. Anything larger is a restart artefact or a
    // driver-level counter jump, and admitting it corrupts the stored history permanently.
    constexpr quint64 kMaxSampleBytes = 64ull * 1024ull * 1024ull * 1024ull;   // 64 GiB
    if (dDown > kMaxSampleBytes || dUp > kMaxSampleBytes)
        return;

    u.down += dDown;
    u.up += dUp;
    m_appDaysDirty.insert(key.left(7));   // yyyy-MM
}

QVector<UsageTracker::AppUsage> UsageTracker::appUsageOverDays(int days) const
{
    QHash<QString, AppUsage> totals;
    const QDate today = QDate::currentDate();
    const QDate from = today.addDays(-(qMax(1, days) - 1));

    for (auto it = m_appDays.constBegin(); it != m_appDays.constEnd(); ++it) {
        const QDate date = QDate::fromString(it.key(), Qt::ISODate);
        if (!date.isValid() || date < from || date > today)
            continue;
        for (const AppUsage &u : it.value()) {
            AppUsage &acc = totals[u.path];
            if (acc.path.isEmpty())
                acc.path = u.path;
            // Take the name from any record that has one, and never let an empty name
            // overwrite a known one: a sample can arrive before the executable has
            // been resolved, and blanking a good name would show a bare path.
            if (!u.name.isEmpty())
                acc.name = u.name;
            acc.down += u.down;
            acc.up += u.up;
        }
    }

    QVector<AppUsage> out;
    out.reserve(totals.size());
    for (const AppUsage &u : totals) {
        if (u.total() > 0)
            out.append(u);
    }
    std::sort(out.begin(), out.end(),
              [](const AppUsage &a, const AppUsage &b) { return a.total() > b.total(); });
    return out;
}

QVector<UsageTracker::AppUsage> UsageTracker::appUsageOn(const QDate &date) const
{
    QVector<AppUsage> out;
    const auto it = m_appDays.constFind(date.toString(Qt::ISODate));
    if (it == m_appDays.constEnd())
        return out;
    for (const AppUsage &u : it.value()) {
        if (u.total() > 0)
            out.append(u);
    }
    std::sort(out.begin(), out.end(),
              [](const AppUsage &a, const AppUsage &b) { return a.total() > b.total(); });
    return out;
}

void UsageTracker::pruneAppHistory(int keepDays)
{
    // keepDays of 0 legitimately means "retain nothing"; the previous qMax(1, ...)
    // silently kept a day, so the function could never clear the history.
    const QDate cutoff = QDate::currentDate().addDays(-qMax(0, keepDays));
    for (auto it = m_appDays.begin(); it != m_appDays.end();) {
        const QDate date = QDate::fromString(it.key(), Qt::ISODate);
        const bool expired = keepDays <= 0 ? true : (date.isValid() && date < cutoff);
        if (date.isValid() && expired) {
            m_appDaysDirty.insert(it.key().left(7));   // rewrite that month without it
            it = m_appDays.erase(it);
        } else {
            ++it;
        }
    }
}

QString UsageTracker::fileForMonth(const QDate &month) const
{
    return QStringLiteral("%1/usage-%2.json")
        .arg(m_dir, month.toString(QStringLiteral("yyyy-MM")));
}

void UsageTracker::load(const UsageConfig &config)
{
    m_days.clear();
    m_dirtyMonths.clear();
    m_appDays.clear();
    m_appDaysDirty.clear();
    m_liveDown = m_liveUp = 0;
    m_today = QDate::currentDate();

    // The cycle can straddle a month boundary, so load every month it touches.
    //
    // The per-app history is loaded over the same window in the same pass: it is
    // shown for the cycle and for the last 7 days, and 7 days can also straddle a
    // month boundary, so the window is widened by a week when the cycle is shorter
    // than that.
    const QDate start = cycleStartDate(config);
    const QDate end = cycleEndDate(config);

    const QDate appFrom = qMin(start, QDate::currentDate().addDays(-6));
    QDate cursor(appFrom.year(), appFrom.month(), 1);
    const QDate lastMonth(end.year(), end.month(), 1);
    int guard = 0;
    while (cursor <= lastMonth && guard++ < 24) {
        loadMonth(cursor);
        loadAppsMonth(cursor);
        cursor = cursor.addMonths(1);
    }

    // Seed the live counters from history, restricted to the active cycle.
    // Without this, cycleTotalBytes() would report only this session's traffic
    // and the 30-day panel would appear to reset on every restart.
    const QDate today = QDate::currentDate();
    for (QDate d = start; d <= today; d = d.addDays(1)) {
        const auto it = m_days.constFind(keyFor(d));
        if (it == m_days.constEnd())
            continue;
        m_liveDown += it.value().down;
        m_liveUp += it.value().up;
    }

    emit usageChanged();
}

void UsageTracker::loadMonth(const QDate &month)
{
    QFile file(fileForMonth(month));
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("Flowbit: ignoring corrupt usage file %s (%s)",
                 qPrintable(file.fileName()), qPrintable(err.errorString()));
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray days = root.value(QStringLiteral("days")).toArray();
    for (const QJsonValue &v : days) {
        const QJsonObject o = v.toObject();
        const QDate date =
            QDate::fromString(o.value(QStringLiteral("date")).toString(), Qt::ISODate);
        if (!date.isValid())
            continue;
        DayUsage u;
        u.date = date;
        u.down = toBytes(o.value(QStringLiteral("down")));
        u.up = toBytes(o.value(QStringLiteral("up")));
        m_days.insert(keyFor(date), u);
    }
}

void UsageTracker::storeMonth(const QDate &month)
{
    QDir().mkpath(m_dir);

    QJsonArray days;
    const QString prefix = month.toString(QStringLiteral("yyyy-MM"));
    for (auto it = m_days.constBegin(); it != m_days.constEnd(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("date"), it.value().date.toString(Qt::ISODate));
        // Stored as an exact integer value (see toBytes for the 2^53 rationale).
        o.insert(QStringLiteral("down"), double(it.value().down));
        o.insert(QStringLiteral("up"), double(it.value().up));
        days.append(o);
    }

    QJsonObject root;
    root.insert(QStringLiteral("days"), days);
    root.insert(QStringLiteral("savedAt"),
                QDateTime::currentDateTime().toString(Qt::ISODate));

    // QSaveFile writes to a temp file and renames, so a crash or power loss
    // mid-write cannot corrupt the counters we depend on for the 30-day view.
    QSaveFile file(fileForMonth(month));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("Flowbit: cannot write usage file %s", qPrintable(file.fileName()));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit())
        qWarning("Flowbit: failed to commit usage file %s", qPrintable(file.fileName()));
}

void UsageTracker::addSample(quint64 downBytes, quint64 upBytes)
{
    if (downBytes == 0 && upBytes == 0)
        return;

    const QDate now = QDate::currentDate();
    if (now != m_today) {
        // Midnight rollover: bank the previous day before attributing anything
        // to the new one.
        flush();
        m_today = now;
    }

    m_liveDown += downBytes;
    m_liveUp += upBytes;

    // Fold into today's bucket as we go, so cycleDays() is accurate even before
    // the next flush.
    DayUsage &today = m_days[keyFor(m_today)];
    if (!today.date.isValid())
        today.date = m_today;
    today.down += downBytes;
    today.up += upBytes;

    m_dirtyMonths.insert(m_today.toString(QStringLiteral("yyyy-MM")));

    emit usageChanged();
}

void UsageTracker::flush()
{
    for (const QString &monthKey : std::as_const(m_dirtyMonths)) {
        const QDate month = QDate::fromString(monthKey + QStringLiteral("-01"), Qt::ISODate);
        if (month.isValid())
            storeMonth(month);
    }
    m_dirtyMonths.clear();

    // The per-app history is written from the same call and the same timer, so a
    // single flush leaves both files consistent with each other.
    for (const QString &monthKey : std::as_const(m_appDaysDirty)) {
        const QDate month = QDate::fromString(monthKey + QStringLiteral("-01"), Qt::ISODate);
        if (month.isValid())
            storeAppsMonth(month);
    }
    m_appDaysDirty.clear();
}

quint64 UsageTracker::cycleDownBytes() const
{
    return m_liveDown;
}

quint64 UsageTracker::cycleUpBytes() const
{
    return m_liveUp;
}

quint64 UsageTracker::cycleTotalBytes() const
{
    return m_liveDown + m_liveUp;
}

QVector<UsageTracker::DayUsage> UsageTracker::cycleDays(const UsageConfig &config) const
{
    QVector<DayUsage> out;

    const QDate start = cycleStartDate(config);
    const QDate end = QDate::currentDate();
    if (!start.isValid() || start > end)
        return out;

    for (QDate d = start; d <= end; d = d.addDays(1)) {
        const auto it = m_days.constFind(keyFor(d));
        DayUsage u;
        u.date = d;
        if (it != m_days.constEnd()) {
            u.down = it.value().down;
            u.up = it.value().up;
        }
        out.append(u);
    }
    return out;
}

QDate UsageTracker::cycleStartDate(const UsageConfig &config)
{
    if (config.cycleStart.isValid())
        return config.cycleStart;
    return QDate::currentDate();
}

QDate UsageTracker::cycleEndDate(const UsageConfig &config)
{
    const QDate start = cycleStartDate(config);
    switch (config.cycle) {
    case CycleKind::Monthly:
        return start.addMonths(1).addDays(-1);
    case CycleKind::Rolling30:
        break;
    }
    return start.addDays(29);
}

QDate UsageTracker::cycleEnd(const UsageConfig &config) const
{
    return cycleEndDate(config);
}

int UsageTracker::daysUntilReset(const UsageConfig &config) const
{
    const QDate end = cycleEndDate(config);
    const qint64 days = QDate::currentDate().daysTo(end);
    return int(std::max<qint64>(0, days));
}

void UsageTracker::resetCurrentCycle(const UsageConfig &config)
{
    const QDate start = cycleStartDate(config);
    const QDate today = QDate::currentDate();

    // Clear only the days inside the active cycle - older history stays intact.
    for (QDate d = start; d <= today; d = d.addDays(1)) {
        const QString k = keyFor(d);
        if (m_days.contains(k)) {
            m_days[k].down = 0;
            m_days[k].up = 0;
            m_dirtyMonths.insert(d.toString(QStringLiteral("yyyy-MM")));
        }
    }

    m_liveDown = m_liveUp = 0;
    flush();

    emit usageChanged();
}

} // namespace net
