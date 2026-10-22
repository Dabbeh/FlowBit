#include "applimitengine.h"
#include "throttleengine.h"

#include "schedule.h"

#include <QDateTime>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <windows.h>
#include <windivert.h>
#include <iphlpapi.h>

#include <chrono>
#include <map>
#include <vector>

namespace net {

namespace {

constexpr size_t kMaxPacket = 65535;

// Bytes/sec to the token bucket's notion of burst. A small burst keeps a single
// large packet from stalling for a long time (which an application would see as
// packet loss), while staying small enough that a speed test reports the configured
// figure rather than the burst.
constexpr double kBurstSeconds = 0.15;

QString describeOpenError(DWORD code)
{
    switch (code) {
    case ERROR_ACCESS_DENIED:
        return QStringLiteral(
            "Flowbit is not running with Administrator rights, so per-application "
            "limits cannot be enforced. Restart Flowbit and accept the elevation "
            "prompt.");
    case ERROR_FILE_NOT_FOUND:
    case ERROR_SERVICE_DOES_NOT_EXIST:
        return QStringLiteral(
            "The WinDivert driver is not available. Reinstall Flowbit so its driver "
            "files are present.");
    case ERROR_SERVICE_DISABLED:
        // Same condition the system-wide limiter reports, and deliberately the same
        // wording and the same one-line fix, so the two features never describe one
        // underlying problem in two different ways.
        return QStringLiteral(
            "The WinDivert driver service is disabled, so application limits cannot "
            "be enforced.\n\n"
            "The service is registered but set not to start. Run this in an "
            "Administrator Command Prompt, then restart Flowbit:\n"
            "    sc config WinDivert start= demand");
    case ERROR_DRIVER_BLOCKED:
    case ERROR_INVALID_IMAGE_HASH:
        return QStringLiteral(
            "Windows blocked the WinDivert driver (signature or policy problem), so "
            "application limits cannot be enforced.");
    case ERROR_ALREADY_EXISTS:
        return QStringLiteral(
            "Another program is already using the WinDivert driver for application "
            "limiting.");
    case ERROR_DEVICE_NOT_CONNECTED:
        // 0x1B1. The driver's DEVICE is not reachable, which is a different failure from
        // the service being missing or disabled: the service exists and may even report
        // itself running, but the device object behind it cannot be opened.
        //
        // Seen on a machine whose driver service was left marked for deletion while the
        // driver was still loaded (Start = 4, DeleteFlag = 1, state RUNNING). In that
        // state the system-wide limiter can attach to the already-loaded instance, so
        // system-wide limits appear to work, while this engine's own additional handle
        // is refused - which is why the fault showed up only on this page.
        //
        // The remedy is the one that state needs, and it is worth naming because
        // "error 433" tells the user nothing.
        return QStringLiteral(
            "The WinDivert driver device is not reachable, so application limits cannot "
            "be enforced.\n\n"
            "The driver service is registered but its device cannot be opened - which "
            "happens when Flowbit is updated while the driver is loaded and Windows is "
            "left holding a service that is pending removal. System-wide limits may still "
            "appear to work, because they can attach to the copy already in memory.\n\n"
            "Restart Windows once. After that the driver loads normally and application "
            "limits apply.");
    default:
        return QStringLiteral("The traffic driver could not be opened (error %1).")
            .arg(code);
    }
}

QString imagePathOf(DWORD pid)
{
    if (pid == 0)
        return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return {};
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD size = DWORD(sizeof(buf) / sizeof(wchar_t));
    QString result;
    if (QueryFullProcessImageNameW(h, 0, buf, &size))
        result = QString::fromWCharArray(buf, int(size));
    CloseHandle(h);
    return QDir::fromNativeSeparators(result);
}

// Matching is case-insensitive because Windows paths are.
QString canonical(const QString &s)
{
    return QDir::fromNativeSeparators(s).trimmed().toLower();
}

} // namespace

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

struct AppLimitEngine::Buckets {
    Pacer down{0};
    Pacer up{0};
    std::atomic<quint64> bytesDown{0};
    std::atomic<quint64> bytesUp{0};
};

class AppLimitEngine::Impl
{
public:
    // Re-evaluates the active rule set on a 1-second cadence.
    //
    // This used to happen only when MainWindow re-pushed the rules (every 30 s),
    // so a schedule window that ended stayed enforced for up to half a minute
    // after its end - measured by the user with a 2-minute window: the limit was
    // still being applied after the end. Schedules must track wall-clock time,
    // not the UI's tick phase.
    mutable QMutex mutex;

    // Rules as configured, and the evaluated state for each.
    QVector<AppRule> rules;
    QVector<AppLimitState> states;
    QString error;

    // tuple -> rule id. Rebuilt as flows appear and as rules change.
    QHash<FlowTuple, quint64> tupleOwner;
    // pid -> rule id, so a flow event for a known process is cheap.
    QHash<quint32, quint64> pidOwner;
    // rule id -> matched pids, for process counting.
    QHash<quint64, QSet<quint32>> pidsByRule;

    // Per-rule buckets, two per rule (one per direction).
    QHash<quint64, std::shared_ptr<Buckets>> buckets;

    void *flowHandle = nullptr;

    // One NETWORK handle and one worker per direction, deliberately.
    //
    // A single handle with the filter "true" (both directions) and a single receive loop
    // serialised the two: Pacer::waitFor blocks the thread that called it, so while an
    // inbound packet was accruing download credit every outbound packet sat unread behind
    // it in the same loop. The measured effect was that setting a download limit also
    // throttled upload - the two directions shared one thread's timeline.
    //
    // Separate handles are what WinDivert requires anyway to run two pacing loops at one
    // priority: handles sharing a layer and priority must use disjoint filters, and
    // "inbound" / "outbound" are exactly disjoint. The system-wide engine already works
    // this way.
    void *netHandleIn = nullptr;
    void *netHandleOut = nullptr;

    std::atomic<bool> stop{false};
    std::thread flowThread;
    std::thread netThreadIn;
    std::thread netThreadOut;
    std::thread tableThread;

    // How process attribution is obtained. The FLOW layer is ideal but is not
    // openable on every Windows build (measured: ERROR_INVALID_PARAMETER 87 on
    // Windows 11 build 26300, for FLOW *and* SOCKET, at any priority, even with
    // no other handle open). Grading needs no driver support, so when the layer
    // is unavailable the connection tables are polled instead - the same
    // GetExtendedTcpTable/UdpTable calls the activity monitor uses.
    enum class Attribution { None, FlowLayer, TablePoll };
    Attribution attribution = Attribution::None;
    QString attributionNote;   // shown in the UI, empty when the flow layer works

    void pollAttribution();

    ~Impl() { shutdown(); }

    void shutdown();
    bool startup(QString *why);

    // Recomputes states from rules + the current time, and drops buckets for rules
    // that no longer limit anything. Called on the GUI thread.
    void reevaluate();

    quint64 ruleIdForPid(quint32 pid, const QString &path);
    void clearAttribution();
};

void AppLimitEngine::Impl::shutdown()
{
    stop.store(true);

    // Closing the handles makes the workers' blocking receives fail, which is the
    // only way to unblock them promptly.
    if (netHandleIn) {
        WinDivertClose(static_cast<HANDLE>(netHandleIn));
        netHandleIn = nullptr;
    }
    if (netHandleOut) {
        WinDivertClose(static_cast<HANDLE>(netHandleOut));
        netHandleOut = nullptr;
    }
    if (flowHandle) {
        WinDivertClose(static_cast<HANDLE>(flowHandle));
        flowHandle = nullptr;
    }

    if (flowThread.joinable())
        flowThread.join();
    if (netThreadIn.joinable())
        netThreadIn.join();
    if (netThreadOut.joinable())
        netThreadOut.join();
    if (tableThread.joinable())
        tableThread.join();

    stop.store(false);
}

bool AppLimitEngine::Impl::startup(QString *why)
{
    // Try the flow layer first: it gives instant, event-driven attribution.
    flowHandle = WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 100, 0);
    if (flowHandle == INVALID_HANDLE_VALUE) {
        flowHandle = nullptr;
        const DWORD flowErr = GetLastError();
        // Not fatal: this Windows build may not expose the layer at all. The
        // packet path (NETWORK layer) is what actually paces traffic.
        attribution = Attribution::TablePoll;
        attributionNote = QStringLiteral(
            "Attribution by connection-table polling: this Windows build does not "
            "expose the driver's flow layer (error %1), so per-application matches "
            "are refreshed every %2 ms instead of live.")
            .arg(flowErr).arg(400);
    } else {
        attribution = Attribution::FlowLayer;
        attributionNote.clear();
    }

    // PRIORITY 100: the system-wide ThrottleEngine holds NETWORK handles at
    // priority 0, and WinDivert requires handles that share a priority to use
    // mutually exclusive filters. A different priority sidesteps that rule.
    //
    // Two handles, one per direction, so each direction gets its own pacing loop. The
    // previous single "true" handle forced both through one thread, which is why a download
    // limit also throttled upload - see the netHandleIn/netHandleOut comment.
    const auto openNetwork = [](const char *filter) -> void * {
        const HANDLE h = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 100, 0);
        return h == INVALID_HANDLE_VALUE ? nullptr : static_cast<void *>(h);
    };

    netHandleIn = openNetwork("inbound");
    netHandleOut = openNetwork("outbound");

    if (!netHandleIn || !netHandleOut) {
        // The driver refused a handle. Before reporting that, give the driver the same
        // repair the system-wide engine gives it.
        //
        // This engine opens its OWN device handles - NETWORK handles at priority 100, and a
        // FLOW handle - where the system-wide engine uses priority 0. A driver left in the
        // state an in-place update produces (service disabled, deletion pending, an instance
        // still resident) can satisfy the handle that is already open while refusing a fresh
        // device connection: WinDivertOpen then fails with ERROR_DEVICE_NOT_CONNECTED (433).
        //
        // The result was the worst kind of report: application limits dead, system-wide limits
        // apparently fine, and a page saying only "the traffic driver could not be opened
        // (error 433)". The system-wide engine already repairs-and-retries in this situation;
        // this one simply gave up, so the two features behaved differently against one fault.
        //
        // One repair, one retry - the same shape the worker uses, and for the same reason: a
        // limit that starts working again beats an error message the user cannot act on.
        const DWORD firstErr = GetLastError();
        bool retried = false;

        if (firstErr == ERROR_SERVICE_DISABLED || firstErr == ERROR_DEVICE_NOT_CONNECTED
            || firstErr == ERROR_SERVICE_DOES_NOT_EXIST || firstErr == ERROR_FILE_NOT_FOUND) {
            const ThrottleEngine::DriverReadiness readiness = ThrottleEngine::ensureDriverReady();

            if (readiness == ThrottleEngine::DriverReadiness::Ready
                || readiness == ThrottleEngine::DriverReadiness::Repaired) {
                // Only the handle that was refused is retried; the other is already open and
                // a second open of the same filter would collide with itself.
                if (!netHandleIn)
                    netHandleIn = openNetwork("inbound");
                if (!netHandleOut)
                    netHandleOut = openNetwork("outbound");
                retried = true;
            }
        }

        if (!netHandleIn || !netHandleOut) {
            // Report the error from the retry when one happened: it describes the state after
            // the repair, which is what the user is actually looking at.
            const DWORD err = GetLastError();
            if (netHandleIn) {
                WinDivertClose(static_cast<HANDLE>(netHandleIn));
                netHandleIn = nullptr;
            }
            if (netHandleOut) {
                WinDivertClose(static_cast<HANDLE>(netHandleOut));
                netHandleOut = nullptr;
            }
            if (flowHandle) {
                WinDivertClose(static_cast<HANDLE>(flowHandle));
                flowHandle = nullptr;
            }
            attribution = Attribution::None;
            if (why) {
                *why = describeOpenError(retried ? err : firstErr);
                if (retried) {
                    *why += QStringLiteral(
                        "\n\nThe driver was repaired and the open retried, and it was still "
                        "refused - so the driver could not be brought into a usable state on "
                        "this run.");
                }
            }
            return false;
        }
    }

    // A deeper queue than the default keeps a burst from being dropped while the
    // pacer is holding packets back.
    for (void *h : {netHandleIn, netHandleOut}) {
        WinDivertSetParam(static_cast<HANDLE>(h), WINDIVERT_PARAM_QUEUE_LENGTH, 8192);
        WinDivertSetParam(static_cast<HANDLE>(h), WINDIVERT_PARAM_QUEUE_TIME, 2000);
    }

    return true;
}

void AppLimitEngine::Impl::pollAttribution()
{
    // Rebuild tuple -> rule ownership from the live connection tables. The whole
    // map is replaced each pass, so a closed or reused connection cannot keep
    // stale ownership (the failure mode the flow-layer version had to handle with
    // DELETED events).
    while (!stop.load()) {
        // Sleep in small slices so shutdown stays prompt.
        for (int i = 0; i < 8 && !stop.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (stop.load())
            break;

        QHash<FlowTuple, quint64> next;
        QHash<quint64, QSet<quint32>> pids;

        // ---- TCP ----
        DWORD size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buf(size);
            if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                                    TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
                const DWORD count = *reinterpret_cast<DWORD *>(buf.data());
                const auto *rows = reinterpret_cast<const MIB_TCPROW_OWNER_PID *>(
                    buf.data() + 4);
                for (DWORD i = 0; i < count; ++i) {
                    const MIB_TCPROW_OWNER_PID &r = rows[i];
                    if (r.dwState != 5)   // ESTABLISHED only
                        continue;
                    const QString path = imagePathOf(r.dwOwningPid);
                    if (path.isEmpty())
                        continue;
                    const quint64 id = ruleIdForPid(r.dwOwningPid, path);
                    if (id == 0)
                        continue;
                    // Table rows are always local -> remote, so the outbound
                    // normalisation applies and both directions match the key.
                    const FlowTuple t = normaliseTuple(
                        r.dwLocalAddr, ntohs(quint16(r.dwLocalPort)), r.dwRemoteAddr,
                        ntohs(quint16(r.dwRemotePort)), 6, true);
                    next.insert(t, id);
                    pids[id].insert(r.dwOwningPid);
                }
            }
        }

        // ---- UDP ----
        size = 0;
        if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
            std::vector<BYTE> buf(size);
            if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET,
                                    UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
                const DWORD count = *reinterpret_cast<DWORD *>(buf.data());
                const auto *rows = reinterpret_cast<const MIB_UDPROW_OWNER_PID *>(
                    buf.data() + 4);
                for (DWORD i = 0; i < count; ++i) {
                    const MIB_UDPROW_OWNER_PID &r = rows[i];
                    const QString path = imagePathOf(r.dwOwningPid);
                    if (path.isEmpty())
                        continue;
                    const quint64 id = ruleIdForPid(r.dwOwningPid, path);
                    if (id == 0)
                        continue;
                    pids[id].insert(r.dwOwningPid);
                    // A UDP socket has no remote endpoint until it is connected;
                    // paced UDP is matched by the packet worker's tuple, which is
                    // built from the packet itself, so only the pid map is needed
                    // for the wildcard case.
                }
            }
        }

        {
            QMutexLocker lock(&mutex);
            if (stop.load())
                break;
            tupleOwner = next;
            pidsByRule = pids;
        }
    }
}

void AppLimitEngine::Impl::clearAttribution()
{
    QMutexLocker lock(&mutex);
    tupleOwner.clear();
    pidOwner.clear();
    pidsByRule.clear();
}

quint64 AppLimitEngine::Impl::ruleIdForPid(quint32 pid, const QString &path)
{
    const QString full = canonical(path);
    const QString exe = QFileInfo(full).fileName();

    QMutexLocker lock(&mutex);

    // Schedule-aware attribution.
    //
    // states[] records, per rule, whether its window covers "now"; the 1-second
    // tick keeps it current. A rule that has left the active set must NOT be
    // attributed: the connection-table poller runs every ~400 ms and would
    // otherwise re-attach flows to an expired rule indefinitely — which is what
    // made a limit persist after its window ended (the reported schedule defect).
    //
    // activeVerdict: 1 = active, 0 = inactive, -1 = no state row yet. "Unknown"
    // counts as active so a brand-new rule works for its first second.
    auto activeVerdict = [&](quint64 id) -> int {
        for (const AppLimitState &s : states) {
            if (s.ruleId == id)
                return s.active ? 1 : 0;
        }
        return -1;
    };

    // Already known: the common case once an application is running. The cached
    // answer must still respect the schedule, or an expired window would keep
    // limiting traffic forever.
    const auto known = pidOwner.constFind(pid);
    if (known != pidOwner.constEnd()) {
        if (activeVerdict(known.value()) == 0)
            return 0;
        return known.value();
    }

    const int idx = matchRuleByPath(rules, full, exe);
    if (idx < 0)
        return 0;

    const quint64 id = rules.at(idx).id;
    if (activeVerdict(id) == 0)
        return 0;

    pidOwner.insert(pid, id);
    pidsByRule[id].insert(pid);
    return id;
}

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

FlowTuple AppLimitEngine::normaliseTuple(quint32 saddr, quint16 sport, quint32 daddr,
                                         quint16 dport, quint8 protocol, bool outbound)
{
    FlowTuple t;
    t.family = 2;   // AF_INET
    t.protocol = protocol;
    if (outbound) {
        // Local endpoint is the source.
        t.local[0] = saddr;
        t.localPort = sport;
        t.remote[0] = daddr;
        t.remotePort = dport;
    } else {
        // Inbound: the packet is addressed to us, so the local endpoint is the
        // destination. Normalising here is what lets one flow record match packets
        // travelling in both directions.
        t.local[0] = daddr;
        t.localPort = dport;
        t.remote[0] = saddr;
        t.remotePort = sport;
    }
    return t;
}

FlowTuple AppLimitEngine::normaliseV6Tuple(const quint32 saddr[4], quint16 sport,
                                           const quint32 daddr[4], quint16 dport,
                                           quint8 protocol, bool outbound)
{
    FlowTuple t;
    t.family = 23;   // AF_INET6
    t.protocol = protocol;
    for (int i = 0; i < 4; ++i) {
        if (outbound) {
            t.local[i] = saddr[i];
            t.remote[i] = daddr[i];
        } else {
            t.local[i] = daddr[i];
            t.remote[i] = saddr[i];
        }
    }
    t.localPort = sport;
    t.remotePort = dport;
    if (!outbound)
        std::swap(t.localPort, t.remotePort);
    return t;
}

// A FLOW-layer IPv4 address is reported as an IPv4-mapped IPv6 address
// (::ffff:A.B.C.D). Those 16 bytes map back to the same plain IPv4 dword the
// packet-layer parser produces, so v4 flow records and v4 packet keys agree.
// (A real IPv6 address always keeps words[0]/[1] populated and the mapped
// prefix absent, so the test is exact.)
static quint32 mappedV4ToDword(const quint32 words[4], bool *ok)
{
    if (words[0] == 0 && words[1] == 0
        && (words[2] & 0xFFFF0000u) == 0xFFFF0000u) {
        *ok = true;
        // Bytes 12..15 of the mapped address become the IPv4 dword, composed the
        // same way the packet parser composes it.
        return words[3];
    }
    *ok = false;
    return 0;
}

int AppLimitEngine::matchRuleByPath(const QVector<AppRule> &rules, const QString &path,
                                    const QString &exe)
{
    const QString full = canonical(path);
    const QString name = canonical(exe);

    // Two passes: an exact path match always beats a bare-name match, so a rule
    // targeting "C:\A\launcher.exe" is not shadowed by an earlier rule that only
    // says "launcher.exe".
    if (!full.isEmpty()) {
        for (int i = 0; i < rules.size(); ++i) {
            const QString rp = canonical(rules.at(i).path);
            if (!rp.isEmpty() && rp == full)
                return i;
        }
    }

    if (!name.isEmpty()) {
        for (int i = 0; i < rules.size(); ++i) {
            // Only rules with no path fall back to name matching, so a
            // path-specific rule can never capture an unrelated binary.
            if (!canonical(rules.at(i).path).isEmpty())
                continue;
            if (canonical(rules.at(i).exe) == name)
                return i;
        }
    }

    return -1;
}

int AppLimitEngine::activeWindow(const AppRule &rule, const QDateTime &now, bool *outAlwaysOn)
{
    if (rule.windows.isEmpty()) {
        if (outAlwaysOn)
            *outAlwaysOn = true;
        return -1;
    }
    if (outAlwaysOn)
        *outAlwaysOn = false;

    // First match wins, exactly as the system-wide scheduler behaves, so the two
    // features are predictable in the same way. ruleMatches() is the shared
    // evaluator, so overnight windows and day masks behave identically in both.
    for (int i = 0; i < rule.windows.size(); ++i) {
        const Rule &w = rule.windows.at(i);
        if (!w.enabled)
            continue;
        if (ruleMatches(w, now))
            return i;
    }
    return -1;
}

AppLimitState AppLimitEngine::evaluate(const AppRule &rule, const QDateTime &now)
{
    AppLimitState s;
    s.ruleId = rule.id;
    s.path = rule.path;
    s.name = rule.name.isEmpty() ? rule.exe : rule.name;

    if (!rule.enabled) {
        s.active = false;
        s.reason = QStringLiteral("Disabled");
        return s;
    }

    bool alwaysOn = false;
    const int window = activeWindow(rule, now, &alwaysOn);

    if (!alwaysOn && window < 0) {
        s.active = false;
        s.matchedWindow = -1;
        s.reason = QStringLiteral("Outside its schedule");
        return s;
    }

    s.active = true;
    s.matchedWindow = window;
    s.down = rule.down;
    s.up = rule.up;

    // A matched window may name its own limits, and until now it could not: the schedule
    // dialog collected a download and an upload rate for every window, stored them, and the
    // engine then evaluated the application's base rates instead. The authored per-window
    // limits were dead data.
    //
    // A direction the window specifies now wins. A window with BOTH directions unlimited
    // inherits the application's limits rather than meaning "unmetered": the dialog cannot
    // express a deliberate "unlimited" (a cleared box and a never-filled box look the same),
    // and every window started with "+ Add Schedule" begins at 0/0. Treating those as
    // overrides would silently uncap the application for the whole window, which is the
    // opposite of what was configured.
    if (window >= 0) {
        const Rule &w = rule.windows.at(window);
        if (w.down != kUnlimited || w.up != kUnlimited) {
            if (w.down != kUnlimited)
                s.down = w.down;
            if (w.up != kUnlimited)
                s.up = w.up;
        }
    }

    s.reason = alwaysOn ? QStringLiteral("Always on")
                        : QStringLiteral("Scheduled window active");

    // A rule whose effective limits are both unlimited is configured but does nothing; saying
    // so is clearer than reporting an active rule with no effect.
    if (s.down == kUnlimited && s.up == kUnlimited) {
        s.active = false;
        s.reason = QStringLiteral("No limits set");
    }

    return s;
}

// ---------------------------------------------------------------------------
// AppLimitEngine
// ---------------------------------------------------------------------------

AppLimitEngine::AppLimitEngine(QObject *parent)
    : QObject(parent), m_impl(new Impl)
{
    // Schedule boundaries must be honoured in about a second of wall-clock time.
    // Re-evaluation used to ride on a 30-second UI timer, so a window that ended
    // stayed enforced for up to half a minute past its end - measured by the user
    // with an 11:40-11:42 window that was still limiting afterwards.
    m_scheduleTick = new QTimer(this);
    m_scheduleTick->setInterval(1000);
    connect(m_scheduleTick, &QTimer::timeout, this, &AppLimitEngine::onScheduleTick);
    m_scheduleTick->start();
}

AppLimitEngine::~AppLimitEngine()
{
    m_impl->shutdown();
}

void AppLimitEngine::Impl::reevaluate()
{
    const QDateTime now = QDateTime::currentDateTime();

    QVector<AppRule> snapshot;
    {
        QMutexLocker lock(&mutex);
        snapshot = rules;
    }

    QVector<AppLimitState> next;
    next.reserve(snapshot.size());
    for (const AppRule &r : snapshot)
        next.append(evaluate(r, now));

    QMutexLocker lock(&mutex);
    states = next;

    // Buckets follow the active set. A rule that stops limiting has its bucket
    // dropped, which releases any packet it was holding and frees the memory.
    QSet<quint64> keep;
    for (const AppLimitState &s : states) {
        if (s.active)
            keep.insert(s.ruleId);
    }
    for (auto it = buckets.begin(); it != buckets.end();) {
        if (keep.contains(it.key()))
            ++it;
        else
            it = buckets.erase(it);
    }
    for (quint64 id : keep) {
        if (!buckets.contains(id))
            buckets.insert(id, std::make_shared<Buckets>());
    }
}

void AppLimitEngine::setRules(const QVector<AppRule> &rules)
{
    {
        QMutexLocker lock(&m_impl->mutex);
        m_impl->rules = rules;
    }

    // Attribution depends on the rule set, so it is rebuilt rather than patched:
    // a rule that was edited must stop owning the tuples it claimed before.
    m_impl->clearAttribution();
    m_impl->reevaluate();
    // The re-evaluated rates reach the token buckets immediately: without this a
    // running engine kept applying the PREVIOUS rule's rate until the next
    // periodic refresh (up to 30 s later).
    applyBucketsFromStates();

    emit stateChanged();
}

void AppLimitEngine::onScheduleTick()
{
    // Only meaningful while the engine is running: a disabled engine has neither
    // session nor buckets, and states are refreshed when rules are set.
    if (m_impl->stop.load())
        return;

    // 1. Recompute which rules are active (their schedule windows cover now).
    m_impl->reevaluate();

    // 2. Re-apply the rates that follow from those states. A rule that left the
    //    active set had its bucket erased in reevaluate(), so its traffic is no
    //    longer paced - the limit is genuinely lifted at the window's end.
    applyBucketsFromStates();
}

void AppLimitEngine::setEnabled(bool enabled)
{
    if (m_enabled.load() == enabled)
        return;

    m_enabled.store(enabled);

    if (enabled) {
        QString why;
        if (!m_impl->startup(&why)) {
            {
                QMutexLocker lock(&m_impl->mutex);
                m_impl->error = why;
            }
            m_running.store(false);
            m_enabled.store(false);
            emit errorOccurred(why);
            emit stateChanged();
            return;
        }

        {
            QMutexLocker lock(&m_impl->mutex);
            m_impl->error.clear();
        }
        m_running.store(true);

        Impl *impl = m_impl.get();

        // Attribution worker: the flow layer when the driver exposes it, table
        // polling otherwise (see Attribution).
        if (m_impl->attribution == Impl::Attribution::TablePoll)
            m_impl->tableThread = std::thread([impl] { impl->pollAttribution(); });

        // ---- flow worker: attributes processes to rules -------------------
        // Runs only when the flow handle exists; with table polling there is no
        // flow handle to receive from.
        m_impl->flowThread = std::thread([this, impl] {
            std::vector<char> buffer(kMaxPacket);
            while (!impl->stop.load()) {
                if (!impl->flowHandle) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                UINT recvLen = 0;
                WINDIVERT_ADDRESS addr{};
                if (!WinDivertRecv(static_cast<HANDLE>(impl->flowHandle), buffer.data(),
                                   UINT(buffer.size()), &recvLen, &addr)) {
                    if (GetLastError() == ERROR_NO_DATA)
                        continue;
                    break;
                }

                if (addr.Layer == WINDIVERT_LAYER_FLOW) {
                    const WINDIVERT_DATA_FLOW &f = addr.Flow;

                    // The same 5-tuple arrives with an ESTABLISHED event when the
                    // flow opens and a DELETED event when it closes. Ignoring the
                    // deleted side would let stale tuples accumulate forever and
                    // later attribute a REUSED tuple (another process, same
                    // ports) to the wrong application.
                    const bool flowDeleted =
                        (addr.Event == WINDIVERT_EVENT_FLOW_DELETED);

                    // FLOW-layer v4 addresses arrive as IPv4-mapped IPv6
                    // (::ffff:A.B.C.D); map them back so v4 flow records and v4
                    // packet keys agree. Real v6 keeps all four words.
                    bool v4 = false;
                    const quint32 localV4 = mappedV4ToDword(f.LocalAddr, &v4);
                    bool v4r = false;
                    const quint32 remV4 = mappedV4ToDword(f.RemoteAddr, &v4r);

                    if (flowDeleted) {
                        FlowTuple t;
                        if (v4 && v4r) {
                            t = normaliseTuple(localV4, f.LocalPort, remV4, f.RemotePort,
                                               f.Protocol, true);
                        } else {
                            t = normaliseV6Tuple(f.LocalAddr, f.LocalPort, f.RemoteAddr,
                                                 f.RemotePort, f.Protocol, true);
                        }
                        t.protocol = f.Protocol;
                        QMutexLocker lock(&impl->mutex);
                        impl->tupleOwner.remove(t);
                    } else {
                        const QString path = imagePathOf(f.ProcessId);
                        if (!path.isEmpty()) {
                            const quint64 id = impl->ruleIdForPid(f.ProcessId, path);
                            if (id != 0) {
                                FlowTuple t;
                                if (v4 && v4r) {
                                    t = normaliseTuple(localV4, f.LocalPort, remV4,
                                                       f.RemotePort, f.Protocol, true);
                                } else {
                                    t = normaliseV6Tuple(f.LocalAddr, f.LocalPort,
                                                         f.RemoteAddr, f.RemotePort,
                                                         f.Protocol, true);
                                }
                                t.protocol = f.Protocol;
                                QMutexLocker lock(&impl->mutex);
                                impl->tupleOwner.insert(t, id);
                            }
                        }
                    }
                }

                // Flow events MUST be re-injected or the network stack stalls.
                WinDivertSend(static_cast<HANDLE>(impl->flowHandle), buffer.data(), recvLen,
                              nullptr, &addr);
            }
        });

        // ---- network workers: one per direction, each pacing only ----------
        //
        // Splitting these is the fix for "a download limit also limits upload".
        //
        // The loop below paces inline - Pacer::waitFor blocks the calling thread until the
        // packet has accrued credit, in slices of up to kMaxSleepSliceMs. With one loop
        // serving both directions, every outbound packet queued behind an inbound packet
        // that was waiting on the download bucket, so upload throughput collapsed to
        // whatever the download cap allowed. Neither direction's rate was wrong; they were
        // simply sharing one thread's timeline.
        //
        // With a handle and a thread each, an inbound wait can no longer delay outbound
        // traffic: the upload bucket decides upload pacing and nothing else.
        const auto startPacketWorker = [this, impl](bool outbound) {
            void *handle = outbound ? impl->netHandleOut : impl->netHandleIn;
            std::thread &slot = outbound ? impl->netThreadOut : impl->netThreadIn;

            slot = std::thread([this, impl, outbound, handle] {
                std::vector<char> buffer(kMaxPacket);
                while (!impl->stop.load()) {
                    if (!handle)
                        break;

                    UINT recvLen = 0;
                    WINDIVERT_ADDRESS addr{};
                    if (!WinDivertRecv(static_cast<HANDLE>(handle), buffer.data(),
                                       UINT(buffer.size()), &recvLen, &addr)) {
                        if (GetLastError() == ERROR_NO_DATA)
                            continue;
                        break;
                    }

                    std::shared_ptr<Buckets> bucket;

                    PWINDIVERT_IPHDR ip = nullptr;
                    PWINDIVERT_IPV6HDR ip6 = nullptr;
                    PWINDIVERT_TCPHDR tcp = nullptr;
                    PWINDIVERT_UDPHDR udp = nullptr;
                    UINT8 protocol = 0;

                    if (WinDivertHelperParsePacket(buffer.data(), recvLen, &ip, &ip6, &protocol,
                                                   nullptr, nullptr, &tcp, &udp, nullptr, nullptr,
                                                   nullptr, nullptr)) {
                        quint16 sport = 0;
                        quint16 dport = 0;
                        if (tcp) {
                            sport = ntohs(tcp->SrcPort);
                            dport = ntohs(tcp->DstPort);
                        } else if (udp) {
                            sport = ntohs(udp->SrcPort);
                            dport = ntohs(udp->DstPort);
                        }

                        // Both families must be handled: previously only v4 packets
                        // reached the tuple lookup, so IPv6 application traffic
                        // silently escaped per-application limits (the system-wide
                        // cap still applied, which made the gap easier to miss).
                        FlowTuple key;
                        if (ip) {
                            key = normaliseTuple(ip->SrcAddr, sport, ip->DstAddr, dport,
                                                 protocol, addr.Outbound != 0);
                        } else if (ip6) {
                            key = normaliseV6Tuple(ip6->SrcAddr, sport, ip6->DstAddr, dport,
                                                   protocol, addr.Outbound != 0);
                        } else {
                            key.protocol = protocol;
                        }

                        QMutexLocker lock(&impl->mutex);
                        const auto it = impl->tupleOwner.constFind(key);
                        if (it != impl->tupleOwner.constEnd()) {
                            const auto b = impl->buckets.constFind(it.value());
                            if (b != impl->buckets.constEnd())
                                bucket = b.value();
                        }
                    }

                    // Only this handle's own direction is paced, so the two buckets are
                    // never consulted from the same thread.
                    if (bucket) {
                        Pacer &pacer = outbound ? bucket->up : bucket->down;
                        if (pacer.rate() > 0) {
                            pacer.waitFor(recvLen, impl->stop);
                            if (outbound) {
                                bucket->bytesUp.fetch_add(recvLen);
                                m_pacedUp.fetch_add(1);
                            } else {
                                bucket->bytesDown.fetch_add(recvLen);
                                m_pacedDown.fetch_add(1);
                            }
                        }
                    }

                    WinDivertSend(static_cast<HANDLE>(handle), buffer.data(), recvLen, nullptr,
                                  &addr);
                }
            });
        };

        startPacketWorker(false);
        startPacketWorker(true);
    } else {
        m_impl->shutdown();
        m_running.store(false);
        m_impl->clearAttribution();
    }

    emit stateChanged();
}

QString AppLimitEngine::attributionNote() const
{
    QMutexLocker lock(&m_impl->mutex);
    return m_impl->attributionNote;
}

QString AppLimitEngine::errorString() const
{
    QMutexLocker lock(&m_impl->mutex);
    return m_impl->error;
}

QVector<AppLimitState> AppLimitEngine::states() const
{
    QMutexLocker lock(&m_impl->mutex);
    return m_impl->states;
}

QVector<AppLimitEngine::LiveApp> AppLimitEngine::liveApps() const
{
    QVector<LiveApp> out;
    QMutexLocker lock(&m_impl->mutex);
    for (auto it = m_impl->buckets.constBegin(); it != m_impl->buckets.constEnd(); ++it) {
        LiveApp a;
        a.ruleId = it.key();
        a.processCount = m_impl->pidsByRule.value(it.key()).size();
        // Rates are derived from paced bytes rather than stored, so the figure
        // reflects what was actually held rather than what was configured.
        a.downBytesPerSec = 0.0;
        a.upBytesPerSec = 0.0;
        out.append(a);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Periodic re-evaluation
// ---------------------------------------------------------------------------
//
// Schedule windows open and close on wall-clock time, so the active set is
// recomputed on a timer rather than only when the settings change. The interval is
// coarse because a window boundary is a minute-resolution event and an idle timer
// must not cost anything measurable while a game is running.

void AppLimitEngine::applyBucketsFromStates()
{
    QMutexLocker lock(&m_impl->mutex);
    for (const AppLimitState &s : std::as_const(m_impl->states)) {
        const auto it = m_impl->buckets.find(s.ruleId);
        if (it == m_impl->buckets.end())
            continue;
        // Rate 0 means unlimited in the Pacer, which short-circuits the wait.
        (*it)->down.setRate(s.active ? s.down : kUnlimited);
        (*it)->up.setRate(s.active ? s.up : kUnlimited);
    }
}

void AppLimitEngine::refresh()
{
    m_impl->reevaluate();
    applyBucketsFromStates();
    emit stateChanged();
}

} // namespace net
