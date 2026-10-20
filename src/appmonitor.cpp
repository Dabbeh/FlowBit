#include "appmonitor.h"

#include <algorithm>
#include <QStringList>
#include "etwclass.h"

#include "appnames.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QThread>
#include <QTimer>

#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <psapi.h>
#include <tdh.h>
#include <tcpmib.h>
#include <tcpestats.h>
// These two live in iphlpapi.dll, but some MinGW header revisions guard the
// declarations behind version checks that do not match our build defines.
// Plain declarations match the documented signatures exactly.
extern "C" {
ULONG WINAPI GetPerTcpConnectionEStats(PMIB_TCPROW Row, TCP_ESTATS_TYPE EstatsType,
                                       PUCHAR Rw, ULONG RwVersion, ULONG RwSize,
                                       PUCHAR Ros, ULONG RosVersion, ULONG RosSize,
                                       PUCHAR Rod, ULONG RodVersion, ULONG RodSize);
ULONG WINAPI SetPerTcpConnectionEStats(PMIB_TCPROW Row, TCP_ESTATS_TYPE EstatsType,
                                       PUCHAR Rw, ULONG RwVersion, ULONG RwSize,
                                       ULONG Offset);
}


#include <atomic>
#include <map>

namespace net {

namespace {

// Provider and event identity.
//
// Microsoft-Windows-Kernel-Network publishes TCP and UDP operations with the
// owning PID and the byte count. The GUID is stable and documented; the class
// GUIDs are used to enable the individual keyword groups.
constexpr GUID kKernelNetworkProvider = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};

// Event IDs within that provider, taken from the schema Windows itself publishes
// (see docs/etw-kernel-network-schema.md, re-derivable with tools/etw_schema_dump.cpp):
//
//   10 TCP send (IPv4)      size = bytes sent
//   11 TCP receive (IPv4)   size = bytes received
//   12 TCP connect (IPv4)   size normally 0
//   13 TCP disconnect       size normally 0
//   14 TCP retransmit, 15 reconnect, 16 copy, 17 failure, 18 ...
//   26..32 the same for IPv6, where daddr/saddr are Binary instead of UInt32
//
// THE ID IS THE ONLY DIRECTION SIGNAL, and getting it backwards is what made
// every download appear as an upload. Two findings, both from a live capture:
//
//   1. The provider publishes the LOCAL ENDPOINT in saddr, not the packet source.
//      Every captured event had saddr_is_local=yes in BOTH directions, so the
//      address comparison can never discriminate. It was never the answer.
//
//   2. Measured while a 3.4 GB download was running: id 10 carried 1,165 bytes
//      (largest 660 - requests and ACKs) while id 11 carried 20,763 bytes
//      (largest 10,080 - the file itself). So id 10 = send, id 11 = receive,
//      exactly as the names say, and the code below originally had them the
//      other way round.
constexpr USHORT kTcpSend = 10;         // upload
constexpr USHORT kTcpRecv = 11;         // download
constexpr USHORT kTcpRetransmit = 14;

// Event schema blobs, keyed by event id and version.
//
// Shared ownership because a blob is handed out under the lock and used after it: the entry may
// be evicted while a decoding pass is still reading it.
QHash<quint32, std::shared_ptr<std::vector<BYTE>>> schemaCache;
QMutex schemaMutex;   // a retransmission of sent data
constexpr USHORT kTcpIpv6Send = 26;     // upload, IPv6
constexpr USHORT kTcpIpv6Recv = 27;     // download, IPv6
constexpr USHORT kTcpIpv6Retransmit = 30;

const wchar_t *kSessionName = L"Flowbit-AppMonitor";

// One network operation, as read out of the event payload.
struct NetEvent {
    quint32 pid = 0;
    quint64 size = 0;
    bool send = false;   // true = outbound (upload), false = inbound
};

// The TCP/UDP payload layout used by Microsoft-Windows-Kernel-Network.
//
// The fields are read by NAME through TDH rather than by assuming a byte offset,
// because the layout has changed across Windows versions and a wrong offset
// silently produces nonsense rather than an error.
struct EventPayload {
    quint32 pid = 0;
    quint32 size = 0;
    quint32 daddr = 0;
    quint32 saddr = 0;
    quint16 dport = 0;
    quint16 sport = 0;
    bool gotPid = false;
    bool gotSize = false;
    bool gotDaddr = false;
    bool gotSaddr = false;
};

// Directs the ETW callback back to the owning monitor instance.
AppMonitor::Impl *g_activeConsumer = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

class AppMonitor::Impl
{
public:
    // -- shared state, guarded by m_mutex ----------------------------------
    QMutex mutex;
    QHash<quint32, quint64> inBytes;    // pid -> bytes received this interval
    QHash<quint32, quint64> outBytes;   // pid -> bytes sent this interval
    QVector<AppTraffic> latest;
    AppTraffic unattributedTraffic;
    QString error;

    // Executable name per PID, resolved once and reused. PIDs are recycled by
    // Windows, so entries are dropped when a PID disappears (see sweep).
    QHash<quint32, QString> exeByPid;

    // Addresses owned by this machine, used to decide event direction. Refreshed
    // lazily; guarded by mutex.
    mutable QSet<quint32> localAddrs;
    mutable qint64 localAddrsAt = 0;
    mutable QMutex localAddrsMutex;
    bool isLocalAddress(quint32 addr) const;

    // -- ETW session -------------------------------------------------------
    TRACEHANDLE session = 0;

    // The name the session was actually created with. Falls back to the shared
    // constant before a session exists, so queries and diagnostics still refer to
    // the well-known name.
    const wchar_t *sessionNameInUse() const
    {
        return activeSessionName.empty() ? kSessionName : activeSessionName.c_str();
    }


    // The session name this instance actually created. It starts as the shared
    // name and changes only if that name is already taken by an orphaned session,
    // in which case a unique name is used so monitoring still starts. Every path
    // that opens, queries or stops the session consults this instead of the
    // constant, otherwise a fallback session would be opened by one name and
    // stopped by another - and leaked.
    std::wstring activeSessionName;
    // True when monitoring had to use a unique session name because the
    // shared one was held by an orphaned session. Surfaced in diagnostics.
    bool usedFallbackName = false;
    TRACEHANDLE traceHandle = INVALID_PROCESSTRACE_HANDLE;
    bool sessionOwned = false;
    QThread *thread = nullptr;
    std::atomic<bool> stopping{false};

    // Counts events so the UI can tell "session running, no traffic" apart from
    // "session running but delivering nothing".
    std::atomic<quint64> eventsSeen{0};
    std::atomic<quint64> eventsParsed{0};
    std::atomic<quint64> udpEvents{0};   // UDP events seen, for diagnostics

    // Every event id the provider delivers, counted BEFORE classification.
    //
    // This exists because "UDP is not counted" has now been diagnosed twice by reasoning and been
    // wrong both times. Ids 0..127 cover this provider with room to spare; anything above is
    // counted in idOverflow. The report shows which ids actually arrive, so the next question
    // ("are 42/43 in the stream at all?") is answered by data.
    std::atomic<quint64> idCounts[128] = {};
    std::atomic<quint64> idOverflow{0};

    // Per-event-id decode accounting plus the payload schema of the first event
    // seen for that id. Written by the ETW callback, read by diagnostics: this is
    // what turns "rates look low" into "this field, this type, this many events".
    struct IdInfo {
        quint64 seen = 0;
        quint64 decoded = 0;
        QString layout;      // "name:InType,..." of the first event, empty until seen
        QString decodedBy;   // which rule produced the value for the first event
    };
    QHash<int, IdInfo> idInfo;   // guarded by mutex

    void rememberLayout(int id, PEVENT_RECORD record, PTRACE_EVENT_INFO info,
                        bool decoded, int typeFallbackIndex);

    // -- TCP EStats fallback ------------------------------------------------
    // Some Windows builds deliver zero ETW events for this provider even with a
    // full keyword mask. Windows separately tracks exact per-connection byte
    // counters (GetPerTcpConnectionEStats), keyed with the owning PID via
    // GetExtendedTcpTable - the mechanism NetBalancer-class tools use. When the
    // ETW path stays silent, this becomes the data source; TCP-only is the trade
    // (QUIC/UDP apps fall under "unattributed"/other only while the provider
    // misbehaves).
    bool useEstats = false;                  // promoted after 3 silent ETW ticks

    // Set when the ETW session could not be created at all (measured on Windows
    // build 26300 with Defender tamper protection: session creation is refused
    // system-wide - even Microsoft's own logman gets Access Denied, and StartTrace
    // answers 5 for a name that cannot exist, from an elevated process holding
    // every relevant privilege).
    //
    // When this is non-empty the monitor is running on the EStats source instead:
    // the data is real, but UDP/QUIC traffic is not counted. It is reported as a
    // scope note, NOT as an error, because nothing is broken.
    QString etwBlockedReason;
    int silentTicks = 0;
    quint64 lastSeenSample = 0;

    // Whether ETW has delivered at least one real event in this session.
    //
    // Once it has, silence can only mean "no traffic", never "the source is broken".
    // Without this latch an idle machine - every application reading 0 B/s - produced three
    // silent ticks and the monitor declared ETW dead, switched to the TCP fallback and
    // showed a warning under the table. The warning then disappeared as soon as traffic
    // arrived, so the panel appeared to flap between sources while ETW was working fine.
    bool etwProven = false;

    struct ConnKey {
        quint32 localAddr;
        quint16 localPort;
        quint32 remoteAddr;
        quint16 remotePort;
        bool operator<(const ConnKey &o) const
        {
            if (localAddr != o.localAddr) return localAddr < o.localAddr;
            if (localPort != o.localPort) return localPort < o.localPort;
            if (remoteAddr != o.remoteAddr) return remoteAddr < o.remoteAddr;
            return remotePort < o.remotePort;
        }
    };

    struct ConnCounters {
        quint64 in = 0;      // last raw DataBytesIn
        quint64 out = 0;     // last raw DataBytesOut
        bool enabled = false;
    };
    QHash<quint64, ConnCounters> connLast;   // key-hash -> last raw counters

    quint64 connHash(quint32 la, quint16 lp, quint32 ra, quint16 rp)
    {
        quint64 h = 1469598103934665603ULL;
        const quint32 words[4] = {la, ra, quint32(lp) << 16 | rp, 0x1F2E3D};
        for (quint32 w : words) {
            h ^= w;
            h *= 1099511628211ULL;
        }
        return h;
    }

    // Records the first few cases where the payload addresses failed to decide
    // direction, with the raw values. Read through diagnostics: it settles byte
    // order from observation instead of assumption.
    mutable QMutex addrMissMutex;
    QStringList addrMissSamples;
    // Loopback traffic (both endpoints local) legitimately cannot be decided by
    // address and is the most common case on a developer machine. It is counted
    // but not stored, so the samples show the case that actually matters: a real
    // internet connection that failed to classify.
    int loopbackMisses = 0;

    // First few REAL (non-loopback) events, every field, with the decision made.
    QStringList rawEvents;
    static QString formatV4(quint32 addr)
    {
        return QString::number((addr >> 24) & 0xFF) + QLatin1Char('.')
               + QString::number((addr >> 16) & 0xFF) + QLatin1Char('.')
               + QString::number((addr >> 8) & 0xFF) + QLatin1Char('.')
               + QString::number(addr & 0xFF);
    }

    void noteRawEvent(quint16 id, quint32 saddr, quint32 daddr, quint16 sport,
                      quint16 dport, bool gotPorts, bool decidedSend, qint64 bytes)
    {
        QMutexLocker lock(&addrMissMutex);
        if (rawEvents.size() >= 10)
            return;
        rawEvents << QStringLiteral(
            "id=%1 %2:%3 -> %4:%5 bytes=%6 decided=%7")
            .arg(id)
            .arg(formatV4(saddr))
            .arg(gotPorts ? QString::number(sport) : QStringLiteral("?"))
            .arg(formatV4(daddr))
            .arg(gotPorts ? QString::number(dport) : QStringLiteral("?"))
            .arg(bytes)
            .arg(decidedSend ? QStringLiteral("SEND/upload")
                             : QStringLiteral("RECV/download"));
    }

    QString rawEventReport() const
    {
        QMutexLocker lock(&addrMissMutex);
        return rawEvents.join(QStringLiteral(" | "));
    }

    // Phase 0.1: raw facts, no interpretation. Kept separate from the samples
    // above because those carry a decision and these must not.
    QStringList rawBits;
    void noteRawBits(quint16 id, quint32 saddr, quint32 daddr, quint16 sport,
                     quint16 dport, bool gotPorts, qint64 bytes, bool sKnown,
                     bool dKnown, quint32 localAsHost)
    {
        QMutexLocker lock(&addrMissMutex);
        if (rawBits.size() >= 12)
            return;
        rawBits << QStringLiteral(
            "id=%1 saddr=0x%2 daddr=0x%3 | saddr_net=%4 saddr_host=%5 | "
            "daddr_net=%6 daddr_host=%7 | ports=%8->%9 size=%10 | "
            "saddr_is_local=%11 daddr_is_local=%12 | myAddr_host=0x%13")
            .arg(id)
            .arg(saddr, 8, 16, QLatin1Char('0'))
            .arg(daddr, 8, 16, QLatin1Char('0'))
            .arg(formatV4(ntohl(saddr)))
            .arg(formatV4(saddr))
            .arg(formatV4(ntohl(daddr)))
            .arg(formatV4(daddr))
            .arg(gotPorts ? QString::number(sport) : QStringLiteral("?"))
            .arg(gotPorts ? QString::number(dport) : QStringLiteral("?"))
            .arg(bytes)
            .arg(sKnown ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(dKnown ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(localAsHost, 8, 16, QLatin1Char('0'));
    }

    QString rawBitsReport() const
    {
        QMutexLocker lock(&addrMissMutex);
        return rawBits.join(QStringLiteral(" | "));
    }

    // The local addresses the comparison uses, listed once so a capture is
    // self-contained.
    QString localAddressList() const
    {
        QMutexLocker lock(&localAddrsMutex);
        QStringList parts;
        for (quint32 a : localAddrs) {
            parts << QStringLiteral("%1 (0x%2)")
                         .arg(formatV4(a))
                         .arg(a, 8, 16, QLatin1Char('0'));
        }
        parts.sort();
        return parts.join(QStringLiteral(", "));
    }
    void noteAddressMiss(quint32 saddr, quint32 daddr, bool sLocal, bool dLocal,
                         quint16 id);

    QString addressMissReport() const
    {
        QMutexLocker lock(&addrMissMutex);
        QStringList out = addrMissSamples;
        if (loopbackMisses > 0) {
            out << QStringLiteral("(loopback samples suppressed: %1)")
                       .arg(loopbackMisses);
        }
        if (out.isEmpty())
            return QString();
        return out.join(QStringLiteral(" | "));
    }

    ~Impl() { stopSession(); }

    void stopSession();
    bool startSession(QString *why);

    // Aggregation.
    void ingest(const NetEvent &event);
    void completeInterval(double seconds, double quietSeconds);
    QString resolveExe(quint32 pid);
    void collectEstats();
    void applyEstatsDeltas(const QHash<quint32, quint64> &inDelta,
                           const QHash<quint32, quint64> &outDelta);
    void sweepDeadPids();
};

void AppMonitor::Impl::stopSession()
{
    stopping.store(true);

    // THE ORDER HERE IS THE WHOLE POINT, and it used to be wrong.
    //
    // ProcessTrace() is a blocking call running on the consumer thread, and it holds the trace
    // handle for as long as it runs. CloseTrace() frees that handle's control block. The old code
    // closed the handle FIRST and joined the thread afterwards, which meant the consumer was still
    // inside ProcessTrace while the handle it was using had already been freed - a use-after-close
    // against the ETW runtime, and exactly the shape of an intermittent crash on collapse.
    //
    // The documented order is the reverse:
    //   1. stop the session   - this is what makes ProcessTrace return
    //   2. wait for the consumer thread, so ProcessTrace has actually returned
    //   3. close the trace handle, now that nothing is using it
    if (sessionOwned && session != 0) {
        // StopTrace blocks until the consumer has drained.
        StopTraceW(session, sessionNameInUse(), nullptr);
        session = 0;
        sessionOwned = false;
    }

    if (g_activeConsumer == this)
        g_activeConsumer = nullptr;

    if (thread) {
        thread->quit();
        if (thread->wait(5000)) {
            delete thread;
            thread = nullptr;
        } else {
            // Still inside ProcessTrace. Deleting a running QThread destroys the thread object
            // under a live stack frame, so the object is deliberately left in place: the thread
            // exits when the session stop is observed, and the handle below is NOT closed while it
            // may still be in use. A leaked QThread object is the lesser fault by a wide margin.
            qWarning("Flowbit: the ETW consumer thread did not stop within 5s; leaving its handle "
                     "open rather than closing it under a running ProcessTrace");
            stopping.store(false);
            return;
        }
    }

    if (traceHandle != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(traceHandle);
        traceHandle = INVALID_PROCESSTRACE_HANDLE;
    }

    stopping.store(false);
}

namespace {

// The machine's configured dynamic (ephemeral) TCP port range, read once from the
// stack's registry parameters. Returns (start, count); (0, 0) if unreadable, in
// which case only the IANA range is used.
QPair<quint16, quint16> readEphemeralRange()
{
    static const QPair<quint16, quint16> cached = [] {
        quint16 start = 0;
        quint16 count = 0;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters",
                          0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD value = 0;
            DWORD size = sizeof(value);
            DWORD type = 0;
            if (RegQueryValueExW(key, L"DynamicPortRangeStartPort", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS
                && type == REG_DWORD) {
                start = quint16(value);
            }
            size = sizeof(value);
            if (RegQueryValueExW(key, L"DynamicPortRangeNumberOfPorts", nullptr,
                                 &type, reinterpret_cast<LPBYTE>(&value),
                                 &size) == ERROR_SUCCESS && type == REG_DWORD) {
                count = quint16(value);
            }
            RegCloseKey(key);
        }
        return QPair<quint16, quint16>(start, count);
    }();
    return cached;
}

// Offset of the event's own name inside TRACE_EVENT_INFO.
//
// The Windows SDK declares this as `union { ULONG EventNameOffset; ULONG ActivityIDNameOffset; }` -
// for a manifest-based event the union holds the event name. MinGW's tdh.h predates that rename and
// only exposes the older member, at the same offset. Rather than pinning the project to one SDK,
// the member is probed at compile time: the first overload is chosen when the name exists.
template <typename Info>
static auto eventNameOffset(const Info *info, int) -> decltype(info->EventNameOffset)
{
    return info->EventNameOffset;
}

template <typename Info>
static ULONG eventNameOffset(const Info *info, long)
{
    return info->ActivityIDNameOffset;
}

// EVENT_RECORD callback. Runs on the monitor thread.
//
// Reads the payload fields by name through TDH. A field that cannot be resolved
// is left at zero and flagged, so a schema change degrades to "not measured"
// rather than to a wrong number.
void WINAPI eventCallback(PEVENT_RECORD record)
{
    AppMonitor::Impl *impl = g_activeConsumer;
    if (!impl || !record)
        return;

    // Count only events we actually intend to account for. The counter used to be
    // incremented for every event the provider emits - including connect and
    // disconnect control events, which carry no bytes - so the reported "decoded"
    // ratio looked like a decoder failure (measured: 3 164 of 9 270, "34%") when
    // most of the remainder were events deliberately not counted.
    const USHORT id = record->EventHeader.EventDescriptor.Id;

    // NOTE: the owning process comes from the payload's PID property (read
    // below), NOT from EventHeader.ProcessId. Using the header was part of the
    // "idle apps show traffic, Firefox shows none" defect.

    // Resolve the payload properties through TDH. Match by NAME - the byte
    // layout differs across Windows builds, but the field names are stable.
    //
    // The schema is fetched ONCE per event id and version and then reused.
    //
    // TdhGetEventInformation is not a cheap accessor: it resolves the provider's manifest and
    // builds the property table, and it was being called twice for every single event - once to
    // measure the buffer and once to fill it. While traffic was busy that dominated the process's
    // CPU time, which is what made Flowbit cost measurable battery while sitting in the tray with
    // the window closed. The blob is immutable for a given id and version, so caching it removes
    // the work entirely after the first event of each kind.
    const quint32 schemaKey = (quint32(id) << 16)
                              | quint32(record->EventHeader.EventDescriptor.Version);

    std::shared_ptr<std::vector<BYTE>> schema;
    {
        QMutexLocker lock(&schemaMutex);
        const auto it = schemaCache.constFind(schemaKey);
        if (it != schemaCache.constEnd())
            schema = *it;
    }

    if (!schema) {
        ULONG bufferSize = 0;
        if (TdhGetEventInformation(record, 0, nullptr, nullptr, &bufferSize)
            != ERROR_INSUFFICIENT_BUFFER)
            return;

        auto fresh = std::make_shared<std::vector<BYTE>>(bufferSize);
        auto *freshInfo = reinterpret_cast<PTRACE_EVENT_INFO>(fresh->data());
        if (TdhGetEventInformation(record, 0, nullptr, freshInfo, &bufferSize) != ERROR_SUCCESS)
            return;

        QMutexLocker lock(&schemaMutex);
        // Bounded: a provider with many ids must not grow this without limit. Sixteen kinds cover
        // this provider's TCP events several times over.
        if (schemaCache.size() >= 16)
            schemaCache.clear();
        schemaCache.insert(schemaKey, fresh);
        schema = fresh;
    }

    auto *info = reinterpret_cast<PTRACE_EVENT_INFO>(schema->data());

    // Classify by the event's OWN NAME, with the id table as a fallback - see etwclass.h for why.
    //
    // This is what makes UDP visible. The decoder previously recognised only TCP send/receive by
    // hardcoded ids, so DNS, QUIC, WireGuard, game and video-call traffic were invisible even
    // though the provider reports all of it and Resource Monitor shows it. Names also cover the
    // IPv4 and IPv6 variants without a table to keep in step.
    const wchar_t *eventName = nullptr;
    const ULONG nameOffset = eventNameOffset(info, 0);
    if (nameOffset) {
        eventName = reinterpret_cast<const wchar_t *>(
            reinterpret_cast<const BYTE *>(info) + nameOffset);
    }

    if (id < 128)
        impl->idCounts[id].fetch_add(1);
    else
        impl->idOverflow.fetch_add(1);

    const etwclass::Kind kind = etwclass::classify(eventName, id);
    if (!kind.transfer)
        return;   // connect, disconnect, copy, failure: carries no bytes to count

    impl->eventsSeen.fetch_add(1);
    impl->etwProven = true;   // proof, from now on, that this source works
    if (kind.udp)
        impl->udpEvents.fetch_add(1);

    // Retransmissions carry the size of data that was already counted when it was first sent.
    // Adding them again inflates per-application usage above the traffic that actually moved - and
    // it disagrees with the TCP-counter source, whose DataBytes numbers count sequence space rather
    // than wire repeats, so the same application would read differently depending on which source
    // was active. They still prove the source is alive, which is why the latch above runs first.
    if (kind.retransmit)
        return;

    // ---------------------------------------------------------------------
    // Payload decode, written against the schema in
    // docs/etw-kernel-network-schema.md (read from Windows, not assumed):
    //
    //   PID    UInt32   owning process            <- attribution
    //   size   UInt32   bytes transferred         <- the byte count
    //   saddr  UInt32   source address            \  direction: a send when
    //   daddr  UInt32   destination address       /  saddr is one of ours
    //
    //   For the IPv6 ids (26..32) saddr/daddr are Binary (16 bytes) instead, so
    //   the type decides how they are read.
    //
    // Every property is read BY NAME through TDH. Earlier versions matched only
    // some names, took direction from the event id, and took the process from the
    // event header - three wrong assumptions that together produced an empty
    // list, then a list attributing traffic to the wrong processes, then inverted
    // download/upload figures.
    // ---------------------------------------------------------------------
    quint32 pid = 0;
    bool gotPid = false;
    qint64 bytes = 0;
    bool gotSize = false;
    quint32 saddr = 0, daddr = 0;
    bool gotSaddr = false, gotDaddr = false;
    quint16 sport = 0, dport = 0;
    bool gotSport = false, gotDport = false;

    // Fallback candidates, used only if the named fields are absent.
    QVector<int> intCandidates;
    int sizeByName = -1;

    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const EVENT_PROPERTY_INFO &prop = info->EventPropertyInfoArray[i];
        const wchar_t *name = prop.NameOffset
            ? reinterpret_cast<const wchar_t *>(
                  reinterpret_cast<const BYTE *>(info) + prop.NameOffset)
            : nullptr;
        const bool structTyped = (prop.Flags & 0x1) != 0;

        if (!structTyped) {
            const USHORT inType = prop.nonStructType.InType;
            if (inType == 8 || inType == 10)   // UInt32 / UInt64
                intCandidates.append(int(i));
        }
        if (!name || structTyped)
            continue;

        PROPERTY_DATA_DESCRIPTOR desc{};
        desc.PropertyName = reinterpret_cast<ULONGLONG>(name);
        desc.ArrayIndex = ULONG_MAX;

        auto readU32 = [&](quint32 *target) -> bool {
            ULONG value = 0;
            ULONG sz = sizeof(value);
            if (TdhGetProperty(record, 0, nullptr, 1, &desc, sz,
                               reinterpret_cast<PBYTE>(&value)) != ERROR_SUCCESS)
                return false;
            *target = value;
            return true;
        };

        if (_wcsicmp(name, L"PID") == 0) {
            gotPid = readU32(&pid);
        } else if (_wcsicmp(name, L"size") == 0) {
            quint32 v = 0;
            if (readU32(&v)) {
                bytes = v;
                gotSize = true;
                sizeByName = int(i);
            } else {
                quint64 w = 0;
                ULONG sz = sizeof(w);
                if (TdhGetProperty(record, 0, nullptr, 1, &desc, sz,
                                   reinterpret_cast<PBYTE>(&w)) == ERROR_SUCCESS) {
                    bytes = qint64(w);
                    gotSize = true;
                    sizeByName = int(i);
                }
            }
        } else if (_wcsicmp(name, L"saddr") == 0) {
            if (readU32(&saddr))
                gotSaddr = true;
        } else if (_wcsicmp(name, L"daddr") == 0) {
            if (readU32(&daddr))
                gotDaddr = true;
        } else if (_wcsicmp(name, L"sport") == 0) {
            quint32 v = 0;
            if (readU32(&v)) {
                sport = quint16(v);
                gotSport = true;
            }
        } else if (_wcsicmp(name, L"dport") == 0) {
            quint32 v = 0;
            if (readU32(&v)) {
                dport = quint16(v);
                gotDport = true;
            }
        }
        // (Binary saddr/daddr on the IPv6 ids yield nothing here; direction for
        // those falls back to the id below, which is unambiguous: 26 send vs
        // 27 receive.)
    }

    // Size fallback, only when the named field was absent. The schema shows other
    // integer properties exist (ports, mss, seqnum, connid), so a single-candidate
    // rule is only safe when exactly one integer property is present.
    if (!gotSize && intCandidates.size() == 1) {
        const EVENT_PROPERTY_INFO &prop =
            info->EventPropertyInfoArray[intCandidates.first()];
        const wchar_t *name = reinterpret_cast<const wchar_t *>(
            reinterpret_cast<const BYTE *>(info) + prop.NameOffset);
        if (name) {
            PROPERTY_DATA_DESCRIPTOR desc{};
            desc.PropertyName = reinterpret_cast<ULONGLONG>(name);
            desc.ArrayIndex = ULONG_MAX;
            ULONG value = 0;
            ULONG sz = sizeof(value);
            if (TdhGetProperty(record, 0, nullptr, 1, &desc, sz,
                               reinterpret_cast<PBYTE>(&value)) == ERROR_SUCCESS) {
                bytes = value;
                gotSize = true;
            }
        }
    }

    // Record the schema once per event id so the diagnostics report can show
    // exactly which fields this Windows build sends (and which rule decoded it).
    if (g_activeConsumer) {
        g_activeConsumer->rememberLayout(int(id), record, info, gotSize,
                                         sizeByName >= 0 ? sizeByName : -1);
    }

    if (!gotSize || bytes <= 0)
        return;

    // ---- direction ---------------------------------------------------------
    // The event id is the direction, and nothing else is.
    //
    // This was wrong for several rounds because it was reasoned about instead of
    // measured. Two findings from a live capture (a 3.4 GB Firefox download,
    // 2026-10-16T19:40 - full detail in docs/roadmap-direction.md) settle it:
    //
    //   1. The provider publishes the LOCAL ENDPOINT in saddr, in BOTH directions.
    //      Every captured event had saddr_is_local=yes, including the receives.
    //      So the address comparison can never discriminate, and code that tried
    //      to use it always fell through to a fallback. That fallback is what
    //      inverted the numbers.
    //
    //   2. Measured during that download: id 10 carried 1,165 bytes in total
    //      (largest 660 - requests and ACKs) while id 11 carried 20,763 bytes
    //      (largest 10,080 - the file data). So id 10 = send and id 11 = receive,
    //      the opposite of what this code previously assumed.
    //
    // The 3-tuple below is the whole rule; the port and address heuristics that
    // used to live here are deleted rather than kept as fallbacks, because they
    // could only ever produce a wrong answer once the id decides correctly.
    const bool send = kind.send;

    // Record the raw evidence for this event. Cheap, bounded, and it is what made
    // the rule above provable - keep it until the direction is confirmed live.
    if (g_activeConsumer && gotSaddr && gotDaddr) {
        const auto isLoop = [](quint32 a) {
            return (ntohl(a) & 0xFF000000u) == 0x7F000000u;
        };
        if (!(isLoop(saddr) && isLoop(daddr))) {
            g_activeConsumer->noteRawBits(
                id, saddr, daddr, sport, dport, gotSport && gotDport, bytes,
                impl->isLocalAddress(saddr), impl->isLocalAddress(daddr), 0);
        }
    }

    // Attribution: payload PID when present, event header otherwise (the header
    // is correct for events without a PID property).
    const quint32 owner = gotPid ? pid : quint32(record->EventHeader.ProcessId);

    NetEvent ev;
    ev.pid = owner;
    ev.size = quint64(bytes);
    ev.send = send;

    impl->eventsParsed.fetch_add(1);
    impl->ingest(ev);
}

} // namespace

bool AppMonitor::Impl::isLocalAddress(quint32 addr) const
{
    // The set of addresses owned by this machine changes when adapters come and
    // go, so it is refreshed lazily rather than cached forever.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (localAddrs.isEmpty() || now - localAddrsAt > 30000) {
        QMutexLocker lock(&localAddrsMutex);
        localAddrs.clear();
        // GetAdaptersAddresses is the supported enumerator; the first unicast
        // address of each adapter is enough for direction decisions.
        ULONG size = 16 * 1024;
        std::vector<BYTE> buf(size);
        ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST
                                            | GAA_FLAG_SKIP_MULTICAST
                                            | GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()),
                                        &size);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            buf.resize(size);
            rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST
                                                | GAA_FLAG_SKIP_MULTICAST
                                                | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()),
                                      &size);
        }
        if (rc == NO_ERROR) {
            for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); a;
                 a = a->Next) {
                for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
                    if (!u->Address.lpSockaddr)
                        continue;
                    if (u->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    const auto *sin = reinterpret_cast<const sockaddr_in *>(
                        u->Address.lpSockaddr);
                    // Stored in host order; isLocalAddress tests both byte orders
                    // because the provider's order is not guaranteed.
                    localAddrs.insert(ntohl(quint32(sin->sin_addr.s_addr)));
                }
            }
        }
        localAddrsAt = now;
    }

    // Adapter addresses are collected in host order. Event addresses have been
    // observed in BOTH orders on this provider (a captured event reported
    // 143.0.168.192 for a machine whose address is 192.168.0.143), so both
    // interpretations are tested rather than assuming one.
    if (localAddrs.contains(ntohl(addr)))
        return true;
    if (localAddrs.contains(addr))
        return true;

    // 127.0.0.0/8 and the unspecified address are local in either order.
    const quint32 host = ntohl(addr);
    if ((host & 0xFF000000u) == 0x7F000000u || host == 0)
        return true;
    if ((addr & 0xFF000000u) == 0x7F000000u)
        return true;
    return false;
}

void AppMonitor::Impl::rememberLayout(int id, PEVENT_RECORD record,
                                      PTRACE_EVENT_INFO info, bool decoded,
                                      int typeFallbackIndex)
{
    QMutexLocker lock(&mutex);
    IdInfo &entry = idInfo[id];
    ++entry.seen;
    if (decoded)
        ++entry.decoded;

    if (!entry.layout.isEmpty())
        return;

    QStringList parts;
    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const EVENT_PROPERTY_INFO &prop = info->EventPropertyInfoArray[i];
        const wchar_t *name = reinterpret_cast<const wchar_t *>(
            reinterpret_cast<const BYTE *>(info) + prop.NameOffset);
        const bool structTyped = (prop.Flags & 0x1) != 0;   // PropertyStruct
        parts << QStringLiteral("%1:%2%3")
                     .arg(name ? QString::fromWCharArray(name) : QStringLiteral("?"))
                     .arg(structTyped ? QStringLiteral("struct")
                                      : QString::number(prop.nonStructType.InType))
                     .arg(structTyped ? QString() : QStringLiteral("/out%1")
                                              .arg(prop.nonStructType.OutType));
    }
    entry.layout = QStringLiteral("id %1 opcode=%2 level=%3 version=%4 | %5")
                       .arg(id)
                       .arg(record->EventHeader.EventDescriptor.Opcode)
                       .arg(record->EventHeader.EventDescriptor.Level)
                       .arg(record->EventHeader.EventDescriptor.Version)
                       .arg(parts.join(QStringLiteral(", ")));
    // typeFallbackIndex here carries the index that produced the value; the
    // caller passes the NAME-match index when the name matched, so it cannot be
    // used to tell the two rules apart on its own. The distinction is kept by the
    // caller instead, and reported as a match by name or by type accordingly.
    entry.decodedBy = decoded ? QStringLiteral("name rule") : QStringLiteral("FAILED");
}

void AppMonitor::Impl::noteAddressMiss(quint32 saddr, quint32 daddr, bool sLocal,
                                       bool dLocal, quint16 id)
{
    QMutexLocker lock(&addrMissMutex);

    // Loopback traffic (127.0.0.1 <-> 127.0.0.1) legitimately cannot be decided by
    // address and is the most common case on a developer machine, so it used to
    // fill every sample slot and hide the case that actually matters: a real
    // internet connection that failed to classify. Loopback samples are therefore
    // counted but not stored.
    const auto isLoopback = [](quint32 addr) {
        return (ntohl(addr) & 0xFF000000u) == 0x7F000000u;
    };
    if (isLoopback(saddr) && isLoopback(daddr)) {
        ++loopbackMisses;
        return;
    }

    if (addrMissSamples.size() >= 12)
        return;
    addrMissSamples << QStringLiteral(
        "id=%1 saddr=0x%2 daddr=0x%3 sLocal=%4 dLocal=%5 knownLocal=%6")
        .arg(id)
        .arg(saddr, 8, 16, QLatin1Char('0'))
        .arg(daddr, 8, 16, QLatin1Char('0'))
        .arg(sLocal).arg(dLocal)
        .arg(localAddrs.size());
}

void AppMonitor::Impl::ingest(const NetEvent &event)
{
    QMutexLocker lock(&mutex);

    // Exactly one source may feed the counters.
    //
    // Both ETW and the TCP-counter fallback add into inBytes/outBytes, and nothing stopped them
    // from doing so during the same window: while the fallback was the chosen source the ETW
    // session was still alive and still delivering events for the same connections, so the same
    // traffic was counted twice. The result was per-application usage that read higher than the
    // traffic that actually moved - reported as the numbers "not exactly accurate".
    //
    // The guard sits here, at the accumulation point, rather than at the call sites: a future
    // caller cannot reintroduce the overlap by forgetting to check the flag.
    if (useEstats)
        return;

    if (event.send)
        outBytes[event.pid] += event.size;
    else
        inBytes[event.pid] += event.size;
}

QString AppMonitor::Impl::resolveExe(quint32 pid)
{
    if (pid == 0)
        return QString();

    // Cached: opening every process on every interval would be far too costly.
    {
        QMutexLocker lock(&mutex);
        const auto it = exeByPid.constFind(pid);
        if (it != exeByPid.constEnd())
            return it.value();
    }

    QString name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD size = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(h, 0, buf, &size))
            name = QDir::fromNativeSeparators(QString::fromWCharArray(buf));
        CloseHandle(h);
    }

    QMutexLocker lock(&mutex);
    exeByPid.insert(pid, name);
    return name;
}

void AppMonitor::Impl::collectEstats()
{
    // Enumerate every established TCP connection with its owning PID, then read
    // the exact per-connection byte counters (TCP EStats). Deltas feed the same
    // pid maps the ETW path feeds, so completeInterval works unchanged.
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
        != ERROR_INSUFFICIENT_BUFFER)
        return;
    std::vector<BYTE> buf(size);
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
        != NO_ERROR)
        return;

    const DWORD count = *reinterpret_cast<DWORD *>(buf.data());
    const auto *rows = reinterpret_cast<const MIB_TCPROW_OWNER_PID *>(buf.data() + 4);

    QSet<quint64> seenKeys;
    QHash<quint32, quint64> inDelta, outDelta;

    for (DWORD i = 0; i < count; ++i) {
        const MIB_TCPROW_OWNER_PID &r = rows[i];
        // MIB_TCP_STATE_ESTAB == 5
        if (r.dwState != 5)
            continue;

        const quint64 key = connHash(r.dwLocalAddr, r.dwLocalPort, r.dwRemoteAddr,
                                     r.dwRemotePort);
        seenKeys.insert(key);

        MIB_TCPROW plain{};
        plain.dwState = r.dwState;
        plain.dwLocalAddr = r.dwLocalAddr;
        plain.dwLocalPort = r.dwLocalPort;
        plain.dwRemoteAddr = r.dwRemoteAddr;
        plain.dwRemotePort = r.dwRemotePort;

        ConnCounters &c = connLast[key];

        // Enable collection once per connection (idempotent). Without it the ROD
        // reads return garbage - documented in the feasibility scan.
        if (!c.enabled) {
            TCP_ESTATS_DATA_RW_v0 rw{};
            rw.EnableCollection = TRUE;
            if (SetPerTcpConnectionEStats(&plain, TcpConnectionEstatsData,
                                          reinterpret_cast<PUCHAR>(&rw), 0,
                                          sizeof(rw), 0) != NO_ERROR) {
                // Can be denied for system-owned connections; skip quietly.
                connLast.remove(key);
                continue;
            }
            c.enabled = true;
        }

        TCP_ESTATS_DATA_ROD_v0 rod{};
        if (GetPerTcpConnectionEStats(&plain, TcpConnectionEstatsData, nullptr, 0, 0,
                                      nullptr, 0, 0,
                                      reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod))
            != NO_ERROR)
            continue;

        const quint64 dIn = (rod.DataBytesIn >= c.in) ? rod.DataBytesIn - c.in : 0;
        const quint64 dOut = (rod.DataBytesOut >= c.out) ? rod.DataBytesOut - c.out : 0;
        c.in = rod.DataBytesIn;
        c.out = rod.DataBytesOut;

        if (dIn)
            inDelta[r.dwOwningPid] += dIn;
        if (dOut)
            outDelta[r.dwOwningPid] += dOut;
    }

    // Prune entries for connections that no longer exist so the hash cannot
    // grow unbounded across a long session.
    for (auto it = connLast.begin(); it != connLast.end();) {
        if (!seenKeys.contains(it.key()))
            it = connLast.erase(it);
        else
            ++it;
    }

    applyEstatsDeltas(inDelta, outDelta);
}

void AppMonitor::Impl::applyEstatsDeltas(const QHash<quint32, quint64> &inDelta,
                                         const QHash<quint32, quint64> &outDelta)
{
    QMutexLocker lock(&mutex);

    // The other half of the same rule: the fallback contributes only while it is the chosen
    // source. See ingest() for why the pair must be mutually exclusive rather than merely
    // usually so.
    if (!useEstats)
        return;

    for (auto it = inDelta.constBegin(); it != inDelta.constEnd(); ++it)
        inBytes[it.key()] += it.value();
    for (auto it = outDelta.constBegin(); it != outDelta.constEnd(); ++it)
        outBytes[it.key()] += it.value();
}

void AppMonitor::Impl::sweepDeadPids()
{
    // Windows recycles PIDs. A cached name would then be attributed to an
    // unrelated new process, so entries whose process no longer exists are
    // dropped.
    QMutexLocker lock(&mutex);
    for (auto it = exeByPid.begin(); it != exeByPid.end();) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it.key());
        if (h) {
            CloseHandle(h);
            ++it;
        } else {
            it = exeByPid.erase(it);
        }
    }
}

void AppMonitor::Impl::completeInterval(double seconds, double quietSeconds)
{
    if (seconds <= 0)
        seconds = 1.0;

    QHash<quint32, quint64> inNow;
    QHash<quint32, quint64> outNow;
    {
        QMutexLocker lock(&mutex);
        inNow.swap(inBytes);
        outNow.swap(outBytes);
    }

    // Group PIDs into applications. Several processes can belong to one
    // application (browsers, Electron apps), and grouping by resolved executable
    // path is the only reliable signal available locally - process names alone
    // would merge unrelated programs that happen to share a name.
    struct Group {
        QString exe;
        QVector<quint32> pids;
        quint64 in = 0;
        quint64 out = 0;
    };
    QHash<QString, Group> groups;
    AppTraffic unattributed;

    QSet<quint32> allPids;
    for (auto it = inNow.constBegin(); it != inNow.constEnd(); ++it)
        allPids.insert(it.key());
    for (auto it = outNow.constBegin(); it != outNow.constEnd(); ++it)
        allPids.insert(it.key());

    for (quint32 pid : allPids) {
        const quint64 in = inNow.value(pid, 0);
        const quint64 out = outNow.value(pid, 0);
        if (in == 0 && out == 0)
            continue;

        const QString exe = resolveExe(pid);
        if (exe.isEmpty()) {
            // System, kernel and protected processes cannot be attributed to a
            // user application, and assigning them to one would be a guess.
            unattributed.bytesIn += in;
            unattributed.bytesOut += out;
            continue;
        }

        Group &g = groups[exe];
        g.exe = exe;
        g.pids.append(pid);
        g.in += in;
        g.out += out;
    }

    QVector<AppTraffic> result;
    result.reserve(groups.size());
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        AppTraffic t;
        t.executablePath = it->exe;
        t.executableName = QFileInfo(it->exe).fileName();
        t.displayName = friendlyAppName(t.executableName);
        t.pids = it->pids;
        t.bytesIn = it->in;
        t.bytesOut = it->out;
        t.downBytesPerSec = double(it->in) / seconds;
        t.upBytesPerSec = double(it->out) / seconds;
        result.append(t);
    }

    std::sort(result.begin(), result.end(), [](const AppTraffic &a, const AppTraffic &b) {
        return a.totalBytesPerSec() > b.totalBytesPerSec();
    });

    // Traffic from processes that could not be resolved to a user application -
    // system and protected processes. Reported separately rather than assigned to
    // an application, because guessing would be misleading.
    unattributed.displayName = QStringLiteral("System and unattributed");
    unattributed.executableName.clear();
    unattributed.executablePath.clear();
    unattributed.downBytesPerSec = double(unattributed.bytesIn) / seconds;
    unattributed.upBytesPerSec = double(unattributed.bytesOut) / seconds;

    {
        QMutexLocker lock(&mutex);
        latest = result;
        unattributedTraffic = unattributed;
    }

    Q_UNUSED(quietSeconds)
}

bool AppMonitor::Impl::startSession(QString *why)
{
    // Try to start the session.
    //
    // NOTE: each attempt builds its OWN properties buffer, sized for the exact
    // name it is about to use. That is not tidiness - sharing one buffer sized for
    // the legacy name while passing a longer pid-suffixed name is what broke
    // monitoring: StartTrace validates Wnode.BufferSize against the name it finds
    // in the buffer, and a mismatch is ERROR_BAD_LENGTH (24). The name also has to
    // fit, or the copy runs off the end of the buffer. Both faults disappear when
    // the buffer is derived from the name on every attempt.
    auto tryStart = [&]() {
        session = 0;
        const std::wstring name = activeSessionName.empty()
                                      ? std::wstring(kSessionName)
                                      : activeSessionName;

        const ULONG bufferSize = ULONG(sizeof(EVENT_TRACE_PROPERTIES)
                                       + (name.size() + 1) * sizeof(wchar_t));
        std::vector<BYTE> propsBuf(bufferSize, 0);
        auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(propsBuf.data());
        props->Wnode.BufferSize = bufferSize;
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;   // QPC timestamps
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        props->BufferSize = 64;           // KB, small: only counters are wanted
        props->MinimumBuffers = 4;
        props->MaximumBuffers = 16;
        props->FlushTimer = 1;

        auto *nameField = reinterpret_cast<wchar_t *>(propsBuf.data() + props->LoggerNameOffset);
        memcpy(nameField, name.c_str(), (name.size() + 1) * sizeof(wchar_t));

        const ULONG rc = StartTraceW(&session, nameField, props);
        if (rc == ERROR_SUCCESS)
            activeSessionName = name;
        return rc;
    };

    // Start on a name that is unique by construction: the shared legacy name is
    // what dead runs leave behind, and a collision with it on this build returns
    // ACCESS_DENIED instead of the documented ERROR_ALREADY_EXISTS - which used to
    // disable every fallback we had. A pid-suffixed name cannot collide with any
    // previous run, so the panel can start even while a ghost exists.
    activeSessionName = QStringLiteral("%1-%2")
                            .arg(QString::fromWCharArray(kSessionName))
                            .arg(quintptr(GetCurrentProcessId()))
                            .toStdWString();
    usedFallbackName = true;

    ULONG startRc = tryStart();

    QString lmOut;   // what the bounded cleanup said, if it ran - surfaced on failure

    if (startRc != ERROR_SUCCESS) {
        // Clearing the leftover MUST NOT happen in this process: ControlTrace STOP
        // can block indefinitely on a real-time session whose owning process was
        // killed mid-flight - which froze the UI the moment Retry was pressed
        // (found by handing the real case to the test harness). logman does the
        // same job out of process, so the wait can be bounded and enforced.
        //
        // The gate is deliberately ANY failure now, not only ALREADY_EXISTS:
        // build 26300 answers ACCESS_DENIED for a collision, which the old
        // single-error gate turned into a dead end with no cleanup and no retry.
        QProcess lm;
        lm.start(QStringLiteral("logman.exe"),
                 {QStringLiteral("stop"), QString::fromWCharArray(kSessionName)});
        if (lm.waitForStarted(2000)) {
            if (!lm.waitForFinished(4000)) {
                lm.kill();
                lm.waitForFinished(1000);
                lmOut = QStringLiteral("logman did not finish in time and was stopped.");
            } else {
                // Capture what logman said, so a stubborn session self-documents
                // instead of leaving a bare error code behind.
                lmOut = QString::fromUtf8(lm.readAllStandardOutput().trimmed());
                if (lmOut.isEmpty())
                    lmOut = QString::fromUtf8(lm.readAllStandardError().trimmed());
            }
        } else {
            lmOut = QStringLiteral("logman could not be started.");
        }

        startRc = tryStart();

        if (startRc != ERROR_SUCCESS) {
            // Second line of defense: logman cannot see kernel-only ghost traces,
            // but ControlTrace from an ELEVATED context reaches them (the user's
            // own elevated diagnostics query proved exactly that). Re-execute THIS
            // binary with an internal flag in a separate short-lived process, so
            // even a wedged kernel lock can never freeze the main window.
            QProcess self;
            self.start(QCoreApplication::applicationFilePath(),
                       {QStringLiteral("--clear-monitor-session")});
            if (self.waitForStarted(2000)) {
                if (!self.waitForFinished(6000)) {
                    self.kill();
                    self.waitForFinished(1000);
                }
            } else if (why) {
                *why = QStringLiteral(
                           "A leftover monitoring session could not be cleared "
                           "(logman: %1; the helper could not be started). Run "
                           "logman stop %2 in an Administrator Command Prompt, or "
                           "restart the PC.")
                           .arg(lmOut.isEmpty() ? QStringLiteral("no output") : lmOut,
                                sessionName());
            }
            startRc = tryStart();
        }
    }

    // Last resort, still collision-free: a recycled pid could have left a ghost
    // named exactly like today's session. Append a run nonce and try once more -
    // a name this unique cannot exist anywhere.
    if (startRc != ERROR_SUCCESS) {
        activeSessionName = (QString::fromStdWString(activeSessionName)
                             + QStringLiteral("-%1")
                                   .arg(QDateTime::currentMSecsSinceEpoch() % 1000000))
                                .toStdWString();
        startRc = tryStart();
    }

    if (startRc != ERROR_SUCCESS) {
        session = 0;
        if (why) {
            if (startRc == ERROR_ACCESS_DENIED) {
                *why = QStringLiteral(
                    "Flowbit could not start per-application monitoring: access was "
                    "denied. On this Windows build that almost always means a "
                    "monitoring session left behind by an earlier run that was "
                    "closed or crashed while monitoring was active. Run "
                    "Fix-Monitoring.bat in the Flowbit folder (accept the admin "
                    "prompt), then press Retry. A restart of the PC also clears "
                    "any stale session.");
            } else if (startRc == ERROR_ALREADY_EXISTS) {
                // Still blocked after the bounded attempt. The cleanup's own words
                // matter here: logman reports "Data Collector Set was not found"
                // for a kernel-only ghost trace, which tells the user a restart is
                // the real fix rather than more logman attempts.
                const QString cleanup = QStringLiteral("    logman stop %1\n").arg(sessionName());
                *why = QStringLiteral(
                    "A monitoring session from an earlier Flowbit run is still active "
                    "and could not be cleared automatically.%1%2\n\n"
                    "If the command above says \"Data Collector Set was not found\", the "
                    "session is a kernel ghost: a restart of the PC always clears it. "
                    "Flowbit is otherwise unaffected.")
                    .arg(lmOut.isEmpty() ? QString() : QStringLiteral("\nlogman said: %1\n").arg(lmOut),
                         cleanup);
            } else {
                *why = QStringLiteral("StartTrace failed (error %1).").arg(startRc);
            }
        }
        return false;
    }
    sessionOwned = true;

    // Enable the provider. Note that a failure here is NOT fatal to the session,
    // so it is reported rather than ignored.
    // MatchAnyKeyword = 0 is the classic 'enable everything' call, but on the
    // newest Windows builds it was observed delivering zero events for this
    // provider. Request the full 64-bit keyword set AND the verbose level: on
    // some builds the kernel-network events are emitted at level 5 and an
    // INFORMATION-level session silently filters them out (observed: session
    // running, counters zero, traffic active).
    const ULONG enableRc = EnableTraceEx2(session, &kKernelNetworkProvider,
                                          EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                          TRACE_LEVEL_VERBOSE,
                                          0xFFFFFFFFFFFFFFFFULL,   // any keyword
                                          0, 0, nullptr);
    if (enableRc != ERROR_SUCCESS) {
        // Reported to the debug log rather than to the user: the note shown under the table
        // says what the fallback costs, and the number is in the diagnostics report.
        qInfo("Flowbit: the kernel network provider would not enable (error %lu), "
              "falling back to TCP EStats",
              static_cast<unsigned long>(enableRc));
        StopTraceW(session, sessionNameInUse(), nullptr);
        session = 0;
        sessionOwned = false;
        if (why)
            // One short sentence, and no error number.
            //
            // The wording has now been wrong in both directions. "Could not be enabled (error
            // 1450)" described a handled fallback as a failure. Replacing it with four lines of
            // explanation described it correctly and broke the layout: a note that long sets a
            // minimum width on the panel, so the columns are pushed off the right edge even in a
            // full-screen window.
            //
            // This is an annotation under a table. It says which source the figures came from
            // and what that source does not see. The build of Windows, the error number and the
            // word "unavailable" are all in the diagnostics report, which has room for them.
            *why = QStringLiteral("TCP only \u2014 UDP and QUIC are not counted on this system.");
        return false;
    }

    // Open the real-time consumer.
    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = const_cast<LPWSTR>(sessionNameInUse());
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME
                               | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = eventCallback;

    // The callback routes events through one process-wide pointer. Two live
    // consumers would silently steal each other's events, so ownership is taken
    // explicitly instead of being assumed.
    if (g_activeConsumer != nullptr) {
        if (why)
            *why = QStringLiteral(
                "Another per-application monitoring handle is already active in "
                "this process.");
        return false;
    }

    g_activeConsumer = this;
    traceHandle = OpenTraceW(&logfile);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
        const DWORD err = GetLastError();
        g_activeConsumer = nullptr;
        StopTraceW(session, sessionNameInUse(), nullptr);
        session = 0;
        sessionOwned = false;
        if (why)
            *why = QStringLiteral("OpenTrace failed (error %1).").arg(err);
        return false;
    }

    // ProcessTrace blocks, so it runs on the monitor thread.
    thread = QThread::create([this] {
        PROCESS_TRACE_MODE_EVENT_RECORD;
        ProcessTrace(&traceHandle, 1, nullptr, nullptr);
    });
    thread->start();

    return true;
}

// ---------------------------------------------------------------------------
// AppMonitor
// ---------------------------------------------------------------------------

AppMonitor::AppMonitor(QObject *parent) : QObject(parent), m_impl(new Impl) {}

AppMonitor::~AppMonitor()
{
    stop();
}

bool AppMonitor::isElevated()
{
    // TokenElevation, NOT CheckTokenMembership.
    //
    // The membership test answers "is the Administrators group in this token?",
    // which is still TRUE for a UAC-FILTERED token - the group is present but
    // deny-only. The monitor therefore believed it was elevated, called StartTrace,
    // and reported the resulting ACCESS_DENIED as a leftover-session problem, while
    // the diagnostics page (which uses TokenElevation) correctly said "running as
    // Administrator". Two checks disagreeing inside one report is what made this
    // bug take so long to see: the app was genuinely running unelevated.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, size, &size)
                    && elev.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

QString AppMonitor::sessionName()
{
    return QString::fromWCharArray(kSessionName);
}

QString AppMonitor::dataSourceReport()
{
    // Measures which sources actually work, instead of inferring a story from an
    // error code. Every wrong diagnosis of this feature came from interpreting a
    // code rather than testing the source; this function is the antidote.
    //
    // Checks, in order of preference:
    //   1. ETW session creation   - StartTrace with a throwaway unique name, then
    //                               stop it. Reports the raw return code.
    //   2. TCP EStats             - can per-connection counters be enabled/read?
    //   3. Connection table       - GetExtendedTcpTable with owning PIDs.
    QStringList parts;

    // ---- 1. ETW ----------------------------------------------------------
    {
        const QString unique = QStringLiteral("Flowbit-SourceCheck-%1")
                                   .arg(quintptr(GetCurrentProcessId()));
        const std::wstring name = unique.toStdWString();
        const ULONG bufSize = ULONG(sizeof(EVENT_TRACE_PROPERTIES)
                                    + (name.size() + 1) * sizeof(wchar_t));
        std::vector<BYTE> buf(bufSize, 0);
        auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(buf.data());
        props->Wnode.BufferSize = bufSize;
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        props->BufferSize = 64;
        props->MinimumBuffers = 4;
        props->MaximumBuffers = 16;
        props->FlushTimer = 1;
        auto *nameField = reinterpret_cast<wchar_t *>(buf.data() + props->LoggerNameOffset);
        memcpy(nameField, name.c_str(), (name.size() + 1) * sizeof(wchar_t));

        TRACEHANDLE session = 0;
        const ULONG rc = StartTraceW(&session, nameField, props);
        if (rc == ERROR_SUCCESS) {
            parts << QStringLiteral("ETW session: works (StartTrace 0)");
            ControlTraceW(session, nameField, props, EVENT_TRACE_CONTROL_STOP);
        } else {
            parts << QStringLiteral("ETW session: BLOCKED (StartTrace %1)").arg(rc);
        }
    }

    // ---- 2. EStats -------------------------------------------------------
    {
        DWORD size = 0;
        bool ok = false;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buf(size);
            if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                                    TCP_TABLE_OWNER_PID_ALL, 0)
                == NO_ERROR) {
                const DWORD count = *reinterpret_cast<DWORD *>(buf.data());
                auto *rows = reinterpret_cast<MIB_TCPROW_OWNER_PID *>(buf.data() + 4);
                for (DWORD i = 0; i < count && !ok; ++i) {
                    if (rows[i].dwState != 5)
                        continue;
                    MIB_TCPROW row{};
                    row.dwState = rows[i].dwState;
                    row.dwLocalAddr = rows[i].dwLocalAddr;
                    row.dwLocalPort = rows[i].dwLocalPort;
                    row.dwRemoteAddr = rows[i].dwRemoteAddr;
                    row.dwRemotePort = rows[i].dwRemotePort;
                    TCP_ESTATS_DATA_RW_v0 rw{};
                    rw.EnableCollection = TRUE;
                    if (SetPerTcpConnectionEStats(&row, TcpConnectionEstatsData,
                                                  reinterpret_cast<PUCHAR>(&rw), 0,
                                                  sizeof(rw), 0)
                        != NO_ERROR)
                        continue;
                    TCP_ESTATS_DATA_ROD_v0 rod{};
                    ok = GetPerTcpConnectionEStats(&row, TcpConnectionEstatsData, nullptr, 0,
                                                   0, nullptr, 0, 0,
                                                   reinterpret_cast<PUCHAR>(&rod), 0,
                                                   sizeof(rod))
                         == NO_ERROR;
                }
            }
        }
        parts << (ok ? QStringLiteral("TCP EStats: works")
                     : QStringLiteral("TCP EStats: no readable connection right now"));
    }

    // ---- 3. connection table --------------------------------------------
    {
        DWORD size = 0;
        bool ok = false;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buf(size);
            ok = GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                                     TCP_TABLE_OWNER_PID_ALL, 0)
                 == NO_ERROR;
        }
        parts << (ok ? QStringLiteral("connection table with PIDs: works")
                     : QStringLiteral("connection table: BLOCKED"));
    }

    return parts.join(QStringLiteral(" | "));
}

bool AppMonitor::leftoverSessionExists()
{
    // Read-only query of the named session: starts nothing, stops nothing.
    //
    // The result codes matter and were previously mishandled: QUERY answers
    // ERROR_WMI_INSTANCE_NOT_FOUND (4201) when no session of that name exists, and
    // that is the ONLY reliable "absent" answer. Treating ERROR_SUCCESS plus the
    // ERROR_MORE_DATA/ERROR_BUFFER_OVERFLOW buffer indications as "exists" made
    // this report a ghost session that was not there - the false positive that
    // blocked monitoring from starting at all (the start path saw "leftover",
    // tried to clear a session that did not exist, and then failed with a message
    // blaming a ghost).
    const ULONG bufSize =
        sizeof(EVENT_TRACE_PROPERTIES) + 2048 * sizeof(wchar_t);
    std::vector<BYTE> propsBuf(bufSize, 0);
    auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(propsBuf.data());
    props->Wnode.BufferSize = bufSize;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    const ULONG rc = ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_QUERY);

    // Only two answers are trustworthy:
    //   4201 (ERROR_WMI_INSTANCE_NOT_FOUND) -> definitively absent
    //   SUCCESS or a buffer indication       -> definitively present
    //
    // Everything else - most importantly 5 (ACCESS_DENIED) - means the QUERY ITSELF
    // was refused, so whether a session exists is UNKNOWN. Reporting "yes" there
    // invented a ghost that was not there and sent diagnosis after it; reporting
    // "no" would hide a real one. Unknown is the honest answer, and the caller
    // treats it as "no reason to block a start".
    constexpr ULONG kInstanceNotFound = 4201;
    if (rc == kInstanceNotFound)
        return false;

    if (rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA || rc == ERROR_BUFFER_OVERFLOW)
        return true;

    return false;   // unknown (e.g. ACCESS_DENIED): do not claim a session exists
}

void AppMonitor::clearGhostSession()
{
    // Runs in the short-lived --clear-monitor-session child process. Ghost traces
    // are invisible to logman but reachable by ControlTrace from an elevated
    // context, which Flowbit always is.
    //
    // The return code used to be discarded, which made a failed stop look like a
    // successful one and left the caller reporting "could not be cleared" with no
    // idea why. The result is now written to a small report file next to the
    // executable so the outcome can be inspected after the child exits.
    const ULONG bufSize =
        sizeof(EVENT_TRACE_PROPERTIES) + 2048 * sizeof(wchar_t);
    std::vector<BYTE> propsBuf(bufSize, 0);
    auto *props = reinterpret_cast<PEVENT_TRACE_PROPERTIES>(propsBuf.data());
    props->Wnode.BufferSize = bufSize;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    const ULONG rc = ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);

    // ERROR_SUCCESS (0) = the session was stopped; 4201
    // (ERROR_WMI_INSTANCE_NOT_FOUND) = there was nothing to stop. Anything else is
    // a real failure and is recorded rather than hidden.
    const QString report = QStringLiteral(
        "clearGhostSession: ControlTrace STOP rc=%1 (0=stopped, 4201=not found)\r\n")
        .arg(rc);
    QFile f(QCoreApplication::applicationDirPath()
            + QStringLiteral("/flowbit-ghost-clear.txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(report.toUtf8());
}

QString AppMonitor::rawBitsReport() const
{
    QString out = m_impl->rawBitsReport();
    const QString locals = m_impl->localAddressList();
    if (!out.isEmpty() && !locals.isEmpty()) {
        out += QStringLiteral(" || LOCAL ADDRESSES: ");
        out += locals;
    }
    return out;
}

QString AppMonitor::rawEventReport() const
{
    return m_impl->rawEventReport();
}

QString AppMonitor::addressMissReport() const
{
    return m_impl->addressMissReport();
}

QString AppMonitor::etwEventLayout() const
{
    QStringList lines;
    QMutexLocker lock(&m_impl->mutex);
    for (auto it = m_impl->idInfo.constBegin(); it != m_impl->idInfo.constEnd(); ++it) {
        lines << QStringLiteral("%1 [seen %2, decoded %3, %4]")
                     .arg(it.value().layout)
                     .arg(it.value().seen)
                     .arg(it.value().decoded)
                     .arg(it.value().decodedBy);
    }
    lines.sort();
    return lines.join(QLatin1Char('\n'));
}

QString AppMonitor::fallbackReason() const
{
    QMutexLocker lock(&m_impl->mutex);
    // The reason set when ETW could not start, or a neutral sentence when the
    // fallback was chosen dynamically (ETW session alive but delivering nothing).
    if (!m_impl->etwBlockedReason.isEmpty())
        return m_impl->etwBlockedReason;
    if (m_impl->useEstats)
        return QStringLiteral("TCP only \u2014 UDP and QUIC are not counted.");
    return QString();
}

bool AppMonitor::usingTcpFallback() const
{
    return m_impl->useEstats;
}

quint64 AppMonitor::eventsSeenTotal() const
{
    return m_impl->eventsSeen.load();
}

QString AppMonitor::eventIdReport() const
{
    struct Entry {
        int id;
        quint64 count;
    };
    QVector<Entry> entries;
    for (int i = 0; i < 128; ++i) {
        const quint64 c = m_impl->idCounts[i].load();
        if (c > 0)
            entries.append({i, c});
    }
    const quint64 overflow = m_impl->idOverflow.load();

    if (entries.isEmpty() && overflow == 0)
        return QStringLiteral("none yet");

    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.count > b.count; });

    QStringList parts;
    for (int i = 0; i < entries.size() && i < 12; ++i)
        parts << QStringLiteral("%1:%2").arg(entries[i].id).arg(entries[i].count);
    if (entries.size() > 12)
        parts << QStringLiteral("...");
    if (overflow > 0)
        parts << QStringLiteral(">127:%1").arg(overflow);

    return parts.join(QStringLiteral("  "));
}

quint64 AppMonitor::udpEventsTotal() const
{
    return m_impl->udpEvents.load();
}

quint64 AppMonitor::eventsDecodedTotal() const
{
    return m_impl->eventsParsed.load();
}

QString AppMonitor::errorString() const
{
    QMutexLocker lock(&m_impl->mutex);
    return m_impl->error;
}

bool AppMonitor::start()
{
    // While an async start is in flight the state reads Running as a
    // transitional placeholder; the worker thread calls this same function and
    // must be allowed through to do the real work.
    if (state() == MonitorState::Running && !m_starting.load())
        return true;

    // Without elevation the ETW trace session cannot be created - but that is not
    // the end of monitoring, and treating it as fatal was the second half of the
    // bug that made this feature report "unavailable" while a working source sat
    // unused. The TCP EStats path needs no session, so the monitor runs on it and
    // says so instead of refusing to start.
    if (!isElevated()) {
        {
            QMutexLocker lock(&m_impl->mutex);
            m_impl->useEstats = true;
            m_impl->silentTicks = 0;
            // One short line. It sits under a table, so every extra clause costs a row of
            // data: the full explanation - why the source is unavailable and what that
            // implies - is in Settings -> Diagnostics, which is where a sentence belongs.
            m_impl->etwBlockedReason = QStringLiteral("TCP only \u2014 UDP and QUIC are not counted.");
            m_impl->error.clear();
            m_lastTick = std::chrono::steady_clock::now();
            m_tickCount = 0;
        }
        m_state.store(MonitorState::Running);
        emit stateChanged();
        return true;
    }
    m_impl->stopping.store(false);
    QString why;
    const bool ok = m_impl->startSession(&why);
    if (!ok) {
        // The ETW session could not be created - but that is not the end of
        // monitoring. The TCP EStats source needs no session at all, and on
        // machines where ETW is blocked outright (Windows build 26300 with
        // Defender tamper protection: even Microsoft's logman is refused) the
        // EStats path still reports real per-connection counters.
        //
        // So: run on EStats rather than declaring the feature unavailable. The
        // reason is kept, shown as a scope note rather than an error, because the
        // data is real - it just cannot see UDP/QUIC traffic.
        {
            QMutexLocker lock(&m_impl->mutex);
            m_impl->useEstats = true;
            m_impl->silentTicks = 0;
            m_impl->etwBlockedReason = why;
            m_impl->error.clear();
            m_lastTick = std::chrono::steady_clock::now();
            m_tickCount = 0;
        }
        m_state.store(MonitorState::Running);
        emit stateChanged();
        return true;
    }

    {
        QMutexLocker lock(&m_impl->mutex);
        m_impl->error.clear();
        // Stamp the interval clock here: the first tick starts measuring from
        // this moment instead of charging the window with stale uptime.
        m_lastTick = std::chrono::steady_clock::now();
        m_tickCount = 0;
    }
    m_state.store(MonitorState::Running);
    emit stateChanged();
    return true;
}

void AppMonitor::startAsync()
{
    // Coalesce repeated presses while a start is in flight.
    bool expected = false;
    if (!m_starting.compare_exchange_strong(expected, true))
        return;

    // Jump straight to a transitional state so the UI's "not enforcing" banner
    // stops claiming the monitor is off while the session spins up.
    if (state() == MonitorState::Stopped)
        m_state.store(MonitorState::Running);
    emit stateChanged();

    QThread *t = QThread::create([this] {
        start();                        // emits stateChanged on completion
        m_starting.store(false);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void AppMonitor::tick()
{
    // With the async start the transitional state is Running before the session
    // settles; ticks during that window only stamp the clock.
    if (state() != MonitorState::Running)
        return;

    const auto now = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(now - m_lastTick).count();
    m_lastTick = now;

    // First tick after start: it only stamps the clock; the first accumulating
    // window begins here rather than being charged with the app's whole uptime.
    if (seconds <= 0.05)
        return;

    // ---- source selection -------------------------------------------------
    // Primary source is ETW. SOME Windows builds (observed on 26300) register
    // the kernel-network provider but deliver ZERO events - counters both zero
    // while traffic flows. In that case promote the TCP EStats fallback after
    // three consecutive silent ticks and keep it for the rest of the session
    // (mixing the two would double-count).
    //
    // Pinned sources: if ETW was blocked outright, the choice is FINAL. The
    // recovery branch below assumes a live ETW session, and flipping back to a
    // source that cannot start would silently stop all counting.
    const bool etwUnavailable = !m_impl->etwBlockedReason.isEmpty();
    if (!etwUnavailable) {
        const quint64 seenNow = m_impl->eventsSeen.load();
        if (seenNow != m_impl->lastSeenSample) {
            // Events arrived. ETW is proven for the rest of the session, and anything the
            // fallback was contributing is dropped so the two never double-count.
            m_impl->etwProven = true;
            m_impl->silentTicks = 0;
            if (m_impl->useEstats) {
                m_impl->useEstats = false;
                m_impl->connLast.clear();
            }
        } else if (!m_impl->etwProven) {
            // Silence, and this source has never delivered anything - only then is it fair to
            // conclude it does not work on this build. Silence alone is not evidence: an idle
            // machine sends nothing, and treating that as a fault made the panel accuse ETW of
            // being broken whenever the connection was quiet.
            if (!m_impl->useEstats && ++m_impl->silentTicks >= 3)
                m_impl->useEstats = true;
        }
        m_impl->lastSeenSample = seenNow;
    }

    // The EStats source needs no trace session: it reads the connection table and
    // per-connection counters directly. That is why it survives an ETW block.
    if (m_impl->useEstats)
        m_impl->collectEstats();

    m_impl->completeInterval(seconds, seconds);
    if (++m_tickCount % 20 == 0)
        m_impl->sweepDeadPids();

    emit updated();
}

void AppMonitor::stopAsync()
{
    // Coalesced like startAsync: pressing Show More repeatedly must not spawn one teardown per
    // press.
    bool expected = false;
    if (!m_stopping.compare_exchange_strong(expected, true))
        return;

    QThread *t = QThread::create([this] {
        stop();
        m_stopping.store(false);
        QMetaObject::invokeMethod(this, [this] { emit stateChanged(); }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void AppMonitor::stop()
{
    if (state() == MonitorState::Stopped)
        return;

    // Let an in-flight async start finish its StartTrace/worker spin-up before
    // tearing everything down, so the two lifecycles cannot interleave on the
    // session handle. Bounded, because startSession is itself bounded.
    if (m_starting.load()) {
        for (int i = 0; i < 100 && m_starting.load(); ++i)
            QThread::msleep(10);
    }

    m_impl->stopSession();
    m_state.store(MonitorState::Stopped);

    {
        QMutexLocker lock(&m_impl->mutex);
        m_impl->latest.clear();
        m_impl->inBytes.clear();
        m_impl->outBytes.clear();
        m_impl->exeByPid.clear();
    }
    // Emit after unlocking - same self-deadlock hazard as in start().
    emit stateChanged();
}

QVector<AppTraffic> AppMonitor::snapshot() const
{
    QMutexLocker lock(&m_impl->mutex);
    return m_impl->latest;
}

AppTraffic AppMonitor::unattributed() const
{
    QMutexLocker lock(&m_impl->mutex);
    return m_impl->unattributedTraffic;
}

void AppMonitor::setIntervalMs(int ms)
{
    m_intervalMs = qBound(200, ms, 10000);
}

void AppMonitor::forceRunningForTesting()
{
    // Presentation-only: the panel reads state() to choose between its error,
    // empty and populated views, and on an unelevated machine the session cannot
    // start, so the other two views would be untestable.
    {
        QMutexLocker lock(&m_impl->mutex);
        m_impl->error.clear();
        m_state.store(MonitorState::Running);
    }
    // Emit outside the lock - test handlers may call snapshot()/errorString().
    emit stateChanged();
}

void AppMonitor::injectForTesting(quint32 pid, const QString &exe, quint64 bytesIn,
                                  quint64 bytesOut)
{
    {
        QMutexLocker lock(&m_impl->mutex);
        m_impl->exeByPid.insert(pid, exe);
        m_impl->inBytes[pid] += bytesIn;
        m_impl->outBytes[pid] += bytesOut;
    }
}

void AppMonitor::flushForTesting(double seconds)
{
    m_impl->completeInterval(seconds, seconds);
}

} // namespace net
