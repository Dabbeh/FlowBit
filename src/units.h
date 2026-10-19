#pragma once

#include "settings.h"

#include <QString>
#include <QStringList>

namespace net {

// ---------------------------------------------------------------------------
// Speed units
//
// Internally every limit is bytes/second. The UI lets the user pick a unit per
// field, so these helpers are the single place that conversion happens.
// ---------------------------------------------------------------------------

enum class SpeedUnit {
    BytesPerSec,
    KibPerSec,   // "KB/s"
    MibPerSec,   // "MB/s"
};

// All units in display order, plus their short labels.
QVector<SpeedUnit> allSpeedUnits();
QString speedUnitLabel(SpeedUnit unit);

// Multiply factor from the unit to bytes/second.
double speedUnitFactor(SpeedUnit unit);

// Converts a bytes/sec value into `unit`.
double fromBytesPerSec(RateBps bps, SpeedUnit unit);

// Converts a value expressed in `unit` into bytes/second.
RateBps toBytesPerSec(double value, SpeedUnit unit);

// Chooses the unit that renders `bps` most readably (0 -> MB/s).
SpeedUnit bestUnitFor(RateBps bps);

// Compact human string, e.g. "Uncapped", "1.50 MB/s", "500 KB/s".
QString formatRate(RateBps bps, bool zeroMeansUncapped = true);

// Data volumes for the usage panel, e.g. "142.5 GB".
QString formatBytes(double bytes, int decimals = 1);

// Day-mask helpers for rule rows ("Mon-Fri", "Daily", ...).
QString describeDays(quint8 mask);
QString shortDayName(int index /*0=Mon*/);

} // namespace net
