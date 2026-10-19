#include "speedmonitor.h"

#include <QString>

// _WIN32_WINNT / NTDDI_VERSION are set in Flowbit.pro so the Windows 7+ APIs
// are reachable at all.
//
// Order matters here: netioapi.h guards MIB_IF_ROW2 / GetIfTable2 behind
// `#ifdef _WS2IPDEF_`, and that macro only exists if ws2ipdef.h was included
// first. Including <winsock2.h> then <ws2ipdef.h> before <iphlpapi.h> sets it
// up; otherwise the v2 interface table API silently disappears and the v1
// GetIfTable/MIB_IFROW names are all that remain.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>

namespace net {

namespace {

// Decides whether an interface row represents real, distinct internet traffic.
//
// This matters more than it looks: Windows reports NDIS lightweight-filter
// drivers (WFP, QoS Packet Scheduler, "Native WiFi Filter Driver") as *separate
// interface rows that carry the same byte counters as the adapter underneath
// them*. On a typical Wi-Fi machine that is five extra rows per adapter, so
// summing them all inflates the readout ~5x - and it would also inflate the
// 30-day data-usage totals, which are driven by these same samples.
//
// The reliable discriminator is the FilterInterface flag, which those stacked
// rows set. We additionally require a hardware interface that is connected, and
// exclude loopback/tunnel pseudo-interfaces.
bool isVirtualInterface(const MIB_IF_ROW2 &row)
{
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
        return true;
    if (row.Type == IF_TYPE_TUNNEL)
        return true;

    // Stacked NDIS filter drivers duplicate the parent adapter's counters.
    if (row.InterfaceAndOperStatusFlags.FilterInterface != FALSE)
        return true;

    // Only physical adapters, and only ones actually carrying traffic.
    if (row.InterfaceAndOperStatusFlags.HardwareInterface == FALSE)
        return true;
    if (row.InterfaceAndOperStatusFlags.ConnectorPresent == FALSE)
        return true;

    return false;
}

} // namespace

SpeedMonitor::SpeedMonitor(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &SpeedMonitor::tick);
}

SpeedMonitor::~SpeedMonitor() = default;

void SpeedMonitor::start()
{
    // Resolve the adapter and take a baseline before the first tick, so the very
    // first sample reports a rate rather than zero.
    m_primaryIndex = pickPrimaryInterface();
    Counters baseline;
    if (m_primaryIndex != 0 && readInterface(m_primaryIndex, &baseline))
        m_previous = baseline;
    else
        m_previous = Counters{};

    m_timer.start();
}

void SpeedMonitor::stop()
{
    m_timer.stop();
    m_downBps = 0.0;
    m_upBps = 0.0;
    m_previous = Counters{};
}

bool SpeedMonitor::readInterface(quint64 ifIndex, Counters *out) const
{
    if (!out || ifIndex == 0)
        return false;

    // GetIfEntry2 reads a single adapter by index, which is what the usage
    // accounting wants: one unambiguous source, so a Wi-Fi <-> Ethernet handoff
    // cannot make the totals jump as the set of "up" adapters changes.
    MIB_IF_ROW2 row{};
    row.InterfaceIndex = static_cast<NET_IFINDEX>(ifIndex);
    if (GetIfEntry2(&row) != NO_ERROR)
        return false;
    if (row.OperStatus != IfOperStatusUp)
        return false;

    out->in = row.InOctets;
    out->out = row.OutOctets;
    out->valid = true;
    return true;
}

quint64 SpeedMonitor::pickPrimaryInterface() const
{
    MIB_IF_TABLE2 *table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr)
        return 0;

    // Prefer the up, physical, non-filter adapter carrying the most total
    // traffic: that is the one actually serving the internet.
    quint64 best = 0;
    quint64 bestTotal = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 &row = table->Table[i];
        if (isVirtualInterface(row) || row.OperStatus != IfOperStatusUp)
            continue;

        const quint64 total = row.InOctets + row.OutOctets;
        if (total >= bestTotal) {
            bestTotal = total;
            best = row.InterfaceIndex;
            const QString alias = QString::fromWCharArray(row.Alias);
            const QString desc = QString::fromWCharArray(row.Description);
            m_adapterName = alias.isEmpty() ? desc : alias;
        }
    }
    FreeMibTable(table);

    return best;
}

void SpeedMonitor::resetBaseline()
{
    // Called after the source adapter changes so the first delta on the new
    // adapter is not measured against the old one's counters.
    m_previous = Counters{};
    m_lastDownDelta = 0;
    m_lastUpDelta = 0;
    m_downBps = 0.0;
    m_upBps = 0.0;
}

QString SpeedMonitor::activeAdapterName() const
{
    // pickPrimaryInterface() fills m_adapterName; call it when nothing has been
    // resolved yet so the footer never shows a stale or blank value.
    if (m_primaryIndex == 0)
        pickPrimaryInterface();

    return m_adapterName.isEmpty() ? QStringLiteral("Network") : m_adapterName;
}

void SpeedMonitor::tick()
{
    // Track a single adapter rather than summing every "up" interface. Summing
    // double-counts nothing (the filter removes duplicates) but it does make the
    // totals jump when a second adapter appears or disappears, and it cannot
    // attribute traffic to "the" adapter shown in the UI.
    Counters now;
    if (m_primaryIndex == 0 || !readInterface(m_primaryIndex, &now)) {
        const quint64 previousIndex = m_primaryIndex;
        m_primaryIndex = pickPrimaryInterface();

        if (m_primaryIndex == 0) {
            m_downBps = 0.0;
            m_upBps = 0.0;
            m_lastDownDelta = 0;
            m_lastUpDelta = 0;
            emit sampled(0.0, 0.0);
            return;
        }

        // Switching adapters invalidates the baseline: the new adapter's
        // cumulative counters have nothing to do with the old one's.
        resetBaseline();
        if (!readInterface(m_primaryIndex, &now)) {
            emit sampled(0.0, 0.0);
            return;
        }
        if (previousIndex != m_primaryIndex)
            emit primaryInterfaceChanged(activeAdapterName());
    }

    if (!m_previous.valid) {
        m_previous = now;
        m_lastDownDelta = 0;
        m_lastUpDelta = 0;
        emit sampled(0.0, 0.0);
        return;
    }

    // A decrease means the adapter was reset (Wi-Fi reconnect, driver reload,
    // sleep/wake) and the OS counter restarted. In that case everything the
    // counter now shows accrued since the reset, so it is all real traffic:
    // take `now.in` as the delta rather than throwing those bytes away.
    //
    // Using 0 here - as an earlier version did - silently dropped a whole
    // reconnect's worth of usage from the 30-day total.
    const quint64 down = now.in >= m_previous.in ? (now.in - m_previous.in) : now.in;
    const quint64 up = now.out >= m_previous.out ? (now.out - m_previous.out) : now.out;

    m_lastDownDelta = down;
    m_lastUpDelta = up;
    m_downBps = double(down);
    m_upBps = double(up);
    m_previous = now;

    emit sampled(m_downBps, m_upBps);
}

QString formatRate(double bytesPerSec)
{
    static const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    double value = std::max(0.0, bytesPerSec);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    const int decimals = (unit == 0 || value >= 100.0) ? 0 : 1;
    return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals).arg(QLatin1String(units[unit]));
}

QString formatKbps(qint64 kbps)
{
    if (kbps <= 0)
        return QStringLiteral("Unlimited");
    return formatRate(double(kbps) * 1024.0);
}

} // namespace net
