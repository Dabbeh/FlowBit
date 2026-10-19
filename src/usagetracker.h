#pragma once

#include "settings.h"

#include <QDate>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <cstdint>

namespace net {

// Tracks how much data the machine has transferred over the current quota cycle.
//
// Byte counts are exact 64-bit integers, never floating point: the source is the
// OS's own ULONG64 adapter counters (MIB_IF_ROW2::InOctets/OutOctets) via
// SpeedMonitor, and floating-point accumulation is avoided so a 30-day total
// carries no rounding at all. Conversion to GB happens only for display.
//
// Totals are flushed to a per-day JSON file periodically and on shutdown, so
// usage survives restarts and a hard kill costs at most one flush interval.
class UsageTracker : public QObject
{
    Q_OBJECT

public:
    explicit UsageTracker(QObject *parent = nullptr);

    // Directory holding usage-YYYY-MM.json, defaulting to the app config dir.
    void setDataDirectory(const QString &dir);
    QString dataDirectory() const { return m_dir; }

    // Loads persisted history for the cycle containing today.
    void load(const UsageConfig &config);

    // Adds an exact byte delta measured by the OS counters since the last call.
    void addSample(quint64 downBytes, quint64 upBytes);

    // Forces a write; called periodically and on shutdown.
    void flush();

    // True when there are unflushed changes.
    bool hasPendingWrites() const { return !m_dirtyMonths.isEmpty(); }

    // Total bytes used within the current cycle (history + this session).
    quint64 cycleTotalBytes() const;
    quint64 cycleDownBytes() const;
    quint64 cycleUpBytes() const;

    // One entry per day in the cycle that has recorded usage.
    struct DayUsage {
        QDate date;
        quint64 down = 0;
        quint64 up = 0;
    };
    QVector<DayUsage> cycleDays(const UsageConfig &config) const;

    // Days remaining until the cycle rolls over, and the rollover date.
    int daysUntilReset(const UsageConfig &config) const;
    QDate cycleEnd(const UsageConfig &config) const;

    // ---- per-application history ------------------------------------------
    //
    // One entry per application per day. Used by the Statistics page to answer
    // "how much has Steam used this cycle?".
    struct AppUsage {
        QString path;      // executable path, the stable identity
        QString name;      // friendly name for display, refreshed as it is seen
        quint64 down = 0;
        quint64 up = 0;

        quint64 total() const { return down + up; }
    };

    struct AppDayUsage {
        QDate date;
        QVector<AppUsage> apps;
    };

    // Records one interval's bytes for an application. Called once per sampled
    // interval per application, so it must stay cheap: it only adds to maps.
    void addAppSample(const QString &path, const QString &name,
                      quint64 downBytes, quint64 upBytes);

    // Applications seen in the last `days` days (default: the whole cycle), summed
    // and sorted busiest-first. `days` of 1 means today.
    QVector<AppUsage> appUsageOverDays(int days) const;

    // Everything seen on one specific date.
    QVector<AppUsage> appUsageOn(const QDate &date) const;

    // Removes per-app history older than the rolling window, so the file cannot
    // grow without bound.
    void pruneAppHistory(int keepDays);

    // Clears usage for the current cycle only.
    void resetCurrentCycle(const UsageConfig &config);

signals:
    // Emitted after totals change, so the UI can repaint without polling.
    void usageChanged();

private:
    static QDate cycleStartDate(const UsageConfig &config);
    static QDate cycleEndDate(const UsageConfig &config);
    QString fileForMonth(const QDate &month) const;
    void loadMonth(const QDate &month);
    void storeMonth(const QDate &month);

    // Per-app history, stored in its own file per month.
    void loadAppsMonth(const QDate &month);
    void storeAppsMonth(const QDate &month);
    QString fileForApps(const QDate &month) const;
    
    QString m_dir;
    // key: "yyyy-MM-dd" -> counters
    QHash<QString, DayUsage> m_days;
    // Totals for the active cycle, seeded from history at load().
    // date -> (path -> totals). A map keeps lookups cheap during the per-second
    // tick, which is where this is written.
    QHash<QString, QHash<QString, AppUsage>> m_appDays;   // day ISO -> path -> usage
    QSet<QString> m_appDaysDirty;                         // months needing a write

    quint64 m_liveDown = 0;
    quint64 m_liveUp = 0;
    QDate m_today;
    QSet<QString> m_dirtyMonths;
};

} // namespace net
