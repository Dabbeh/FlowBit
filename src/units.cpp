#include "units.h"

#include <QObject>

namespace net {

namespace {
constexpr double kKi = 1024.0;
constexpr double kMi = 1024.0 * 1024.0;
constexpr double kGi = 1024.0 * 1024.0 * 1024.0;

// Monday-first initials, matching the `days` bitmask in Rule.
const char *kDayShort[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
} // namespace

QVector<SpeedUnit> allSpeedUnits()
{
    return {SpeedUnit::MibPerSec, SpeedUnit::KibPerSec, SpeedUnit::BytesPerSec};
}

QString speedUnitLabel(SpeedUnit unit)
{
    switch (unit) {
    case SpeedUnit::BytesPerSec:
        return QStringLiteral("B/s");
    case SpeedUnit::KibPerSec:
        return QStringLiteral("KB/s");
    case SpeedUnit::MibPerSec:
        return QStringLiteral("MB/s");
    }
    return QStringLiteral("MB/s");
}

double speedUnitFactor(SpeedUnit unit)
{
    switch (unit) {
    case SpeedUnit::BytesPerSec:
        return 1.0;
    case SpeedUnit::KibPerSec:
        return kKi;
    case SpeedUnit::MibPerSec:
        return kMi;
    }
    return kMi;
}

double fromBytesPerSec(RateBps bps, SpeedUnit unit)
{
    return double(bps) / speedUnitFactor(unit);
}

RateBps toBytesPerSec(double value, SpeedUnit unit)
{
    if (value <= 0.0)
        return kUnlimited;
    return RateBps(value * speedUnitFactor(unit) + 0.5);
}

SpeedUnit bestUnitFor(RateBps bps)
{
    if (bps >= RateBps(kMi))
        return SpeedUnit::MibPerSec;
    if (bps >= RateBps(kKi))
        return SpeedUnit::KibPerSec;
    return SpeedUnit::BytesPerSec;
}

QString formatRate(RateBps bps, bool zeroMeansUncapped)
{
    if (bps <= 0)
        return zeroMeansUncapped ? QObject::tr("Uncapped") : QStringLiteral("0 B/s");

    const SpeedUnit unit = bestUnitFor(bps);
    const double value = fromBytesPerSec(bps, unit);

    // Two decimals for MB/s (2.50), none for whole KB/s, one for fractional.
    int decimals = 2;
    if (unit == SpeedUnit::KibPerSec)
        decimals = (value >= 100.0 || value == std::floor(value)) ? 0 : 1;
    else if (unit == SpeedUnit::BytesPerSec)
        decimals = 0;

    return QStringLiteral("%1 %2")
        .arg(value, 0, 'f', decimals)
        .arg(speedUnitLabel(unit));
}

QString formatBytes(double bytes, int decimals)
{
    struct Entry {
        double factor;
        const char *label;
    };
    static const Entry entries[] = {
        {kGi, "GB"}, {kMi, "MB"}, {kKi, "KB"}, {1.0, "B"},
    };

    double value = bytes > 0 ? bytes : 0.0;
    for (const Entry &e : entries) {
        if (value >= e.factor || e.factor == 1.0) {
            const double shown = value / e.factor;
            return QStringLiteral("%1 %2")
                .arg(shown, 0, 'f', decimals)
                .arg(QLatin1String(e.label));
        }
    }
    return QStringLiteral("0 GB");
}

QString shortDayName(int index)
{
    if (index < 0 || index > 6)
        return QString();
    return QString::fromLatin1(kDayShort[index]);
}

QString describeDays(quint8 mask)
{
    if (mask == 0)
        return QObject::tr("Never");
    if (mask == 0x7F)
        return QObject::tr("Daily");
    if (mask == 0x1F)
        return QObject::tr("Mon-Fri");
    if (mask == 0x60)
        return QObject::tr("Sat-Sun");

    // Otherwise list the selected days compactly: "Mon, Wed, Fri".
    QStringList parts;
    for (int i = 0; i < 7; ++i) {
        if (mask & (1u << i))
            parts << shortDayName(i);
    }
    return parts.join(QStringLiteral(", "));
}

} // namespace net
