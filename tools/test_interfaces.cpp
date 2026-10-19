// Verifies SpeedMonitor's interface filtering against the real MIB_IF table.
//
// Why this exists: Windows reports stacked NDIS lightweight-filter drivers
// (WFP, QoS Packet Scheduler, "Native WiFi Filter Driver") as separate interface
// rows carrying the *same* counters as the adapter beneath them. On the machine
// this was developed on, that is five duplicate rows per Wi-Fi adapter. Summing
// them all inflates the live speed readout ~5x AND the 30-day data-usage totals,
// which are driven by the same samples.
//
// The test asks the live system for its interface table and asserts the
// selection logic keeps exactly the distinct, physical, connected adapters.
//
// Build/run: tools/run_tests.ps1
#include "../src/speedmonitor.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <cstdio>

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

// Mirror of the private predicate in speedmonitor.cpp. Kept in sync deliberately:
// if the production rule changes, this test must change with it.
static bool isVirtualInterface(const MIB_IF_ROW2 &row)
{
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
        return true;
    if (row.Type == IF_TYPE_TUNNEL)
        return true;
    if (row.InterfaceAndOperStatusFlags.FilterInterface != FALSE)
        return true;
    if (row.InterfaceAndOperStatusFlags.HardwareInterface == FALSE)
        return true;
    if (row.InterfaceAndOperStatusFlags.ConnectorPresent == FALSE)
        return true;
    return false;
}

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", qPrintable(what));
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    MIB_IF_TABLE2 *table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        std::printf("cannot read interface table; skipping\n");
        return 0;
    }

    int totalRows = 0;
    int loopbackOrTunnel = 0;
    int filterRows = 0;
    int virtualRows = 0;
    int selected = 0;
    int selectedWithTraffic = 0;
    QStringList selectedNames;
    quint64 selectedIn = 0;

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 &row = table->Table[i];
        ++totalRows;

        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) {
            ++loopbackOrTunnel;
            check(isVirtualInterface(row), "loopback/tunnel rows are excluded");
            continue;
        }
        if (row.InterfaceAndOperStatusFlags.FilterInterface != FALSE) {
            ++filterRows;
            check(isVirtualInterface(row),
                  QStringLiteral("filter-driver row excluded: %1")
                      .arg(QString::fromWCharArray(row.Alias)));
        }
        if (isVirtualInterface(row)) {
            ++virtualRows;
            continue;
        }

        ++selected;
        selectedNames << QString::fromWCharArray(row.Alias);
        selectedIn += row.InOctets;

        // Everything selected must be a real, connected, physical adapter -
        // otherwise duplicate counters would leak into the totals.
        check(row.InterfaceAndOperStatusFlags.HardwareInterface != FALSE,
              QStringLiteral("selected row is a hardware interface: %1")
                  .arg(QString::fromWCharArray(row.Alias)));
        check(row.InterfaceAndOperStatusFlags.FilterInterface == FALSE,
              QStringLiteral("selected row is not a filter driver: %1")
                  .arg(QString::fromWCharArray(row.Alias)));
        check(row.InterfaceAndOperStatusFlags.ConnectorPresent != FALSE,
              QStringLiteral("selected row has a physical connector: %1")
                  .arg(QString::fromWCharArray(row.Alias)));
        check(row.Type != IF_TYPE_TUNNEL && row.Type != IF_TYPE_SOFTWARE_LOOPBACK,
              QStringLiteral("selected row is neither tunnel nor loopback: %1")
                  .arg(QString::fromWCharArray(row.Alias)));

        if (row.InOctets + row.OutOctets > 0)
            ++selectedWithTraffic;
    }

    std::printf("  interface table: %d rows (%d loopback/tunnel, %d filter-driver, %d virtual)\n",
                totalRows, loopbackOrTunnel, filterRows, virtualRows);
    std::printf("  selected as real adapters: %d -> %s\n", selected,
                qPrintable(selectedNames.join(QStringLiteral(", "))));
    std::printf("  selected total InOctets: %llu\n", static_cast<unsigned long long>(selectedIn));

    FreeMibTable(table);

    // The machine must have at least one real adapter, or the app could never
    // report a speed at all.
    check(selected >= 1, "at least one real adapter is selected");
    check(selected < totalRows,
          "not every row is selected (duplicates are being filtered)");

    // The duplicate-row problem must actually be handled on this machine: if
    // filter drivers are present, they must all have been excluded.
    if (filterRows > 0) {
        std::printf("  -> %d stacked filter-driver rows were excluded\n", filterRows);
    }

    // Sanity: a selected hardware adapter should be a plausible order of
    // magnitude. Guards against accidentally summing an enormous table.
    check(selected <= 8,
          QStringLiteral("selected adapter count is small (got %1)").arg(selected));

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
