#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

namespace net {

// Reports whole-system throughput by sampling the interface byte counters
// (GetIfTable2) once per second. Loopback, tunnel and stacked filter-driver
// pseudo-interfaces are skipped so the numbers match what the user sees in Task
// Manager and, critically, so duplicated counters are not summed.
//
// The same samples feed the 60-second graph and the data-usage tracker, so a
// single timer drives all three readouts and they cannot disagree.
class SpeedMonitor : public QObject
{
    Q_OBJECT

public:
    explicit SpeedMonitor(QObject *parent = nullptr);
    ~SpeedMonitor() override;

    void start();
    void stop();

    double downBytesPerSec() const { return m_downBps; }
    double upBytesPerSec() const { return m_upBps; }

    // Exact byte deltas from the OS counters for the most recent tick. These are
    // the authoritative numbers for usage accounting: they come straight from
    // the adapter's ULONG64 InOctets/OutOctets, not from rate x time, so no
    // timer drift or micro-burst can be lost.
    quint64 lastDownBytes() const { return m_lastDownDelta; }
    quint64 lastUpBytes() const { return m_lastUpDelta; }

    // Friendly name of the interface currently carrying the most traffic, e.g.
    // "Wi-Fi" - shown in the scheduler footer. Falls back to "Network".
    QString activeAdapterName() const;

    // Interface index of the adapter being read, or 0 when none is usable.
    quint64 primaryInterfaceIndex() const { return m_primaryIndex; }

signals:
    // Emitted once per second with the rate (for the graph) and the exact byte
    // deltas (for usage accounting).
    void sampled(double downBytesPerSec, double upBytesPerSec);

    // Emitted when the source adapter changes (e.g. Wi-Fi -> Ethernet), which
    // lets the usage tracker re-baseline instead of recording a bogus delta.
    void primaryInterfaceChanged(const QString &adapterName);

private slots:
    void tick();

private:
    struct Counters {
        quint64 in = 0;
        quint64 out = 0;
        bool valid = false;
    };

    // Reads one specific adapter via GetIfEntry2. Returns false when the adapter
    // is gone or down, so the caller can re-pick.
    bool readInterface(quint64 ifIndex, Counters *out) const;

    // Chooses the adapter carrying the most traffic. Returns the interface index
    // (0 when none is usable); kept as a plain integer so this header does not
    // need to pull in the Windows networking types.
    quint64 pickPrimaryInterface() const;

    // Clears the baseline after the source adapter changes.
    void resetBaseline();

    QTimer m_timer;
    Counters m_previous;
    double m_downBps = 0.0;
    double m_upBps = 0.0;
    quint64 m_lastDownDelta = 0;
    quint64 m_lastUpDelta = 0;
    quint64 m_primaryIndex = 0;
    mutable QString m_adapterName;
};

} // namespace net
