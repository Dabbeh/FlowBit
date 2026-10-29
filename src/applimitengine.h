#pragma once

#include "settings.h"
#include "throttleengine.h"

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QtGlobal>
#include <QSet>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>
#include <thread>

namespace net {

// A network flow attributed to a process, reduced to what packet matching needs.
//
// The tuple is stored NORMALISED: local endpoint first, remote endpoint second,
// regardless of which direction the flow event described. At the packet layer an
// outbound packet has the local endpoint as its source and an inbound packet has it
// as its destination, so normalising lets one record match both directions - which
// is what makes upload and download limiting work from a single flow observation.
struct FlowTuple {
    // IPv4 and IPv6 share the structure: `family` selects the interpretation
    // (2 = AF_INET, 23 = AF_INET6); v4 leaves all words except local[0] /
    // remote[0] zero, so one hash covers both.
    quint8 family = 2;
    quint32 local[4] = {0, 0, 0, 0};
    quint16 localPort = 0;
    quint32 remote[4] = {0, 0, 0, 0};
    quint16 remotePort = 0;
    quint8 protocol = 0;

    bool operator==(const FlowTuple &o) const
    {
        if (family != o.family || localPort != o.localPort
            || remotePort != o.remotePort || protocol != o.protocol)
            return false;
        for (int i = 0; i < 4; ++i) {
            if (local[i] != o.local[i] || remote[i] != o.remote[i])
                return false;
        }
        return true;
    }
};

inline uint qHash(const FlowTuple &t, uint seed = 0)
{
    // Combine the fields by hand rather than delegating to qHash(): an overload
    // declared in this namespace hides the integer overloads, so calling qHash()
    // here would recurse into this function instead. A 64-bit FNV-style mix over
    // the packed fields is enough for a hash table keyed on 5-tuples.
    quint64 h = 1469598103934665603ULL;
    for (int i = 0; i < 4; ++i) {
        h ^= quint32(t.local[i] ^ (t.remote[i] + i));
        h *= 1099511628211ULL;
    }
    h ^= quint32(t.localPort) << 16 | t.remotePort;
    h *= 1099511628211ULL;
    h ^= quint32(t.protocol) << 8 | quint32(t.family) | seed;
    h *= 1099511628211ULL;
    return uint(h ^ (h >> 32));
}

// What is limited for one application right now.
struct AppLimitState {
    quint64 ruleId = 0;
    QString path;
    QString name;
    RateBps down = kUnlimited;
    RateBps up = kUnlimited;
    bool active = false;        // a rule is currently in force
    int matchedWindow = -1;     // index of the matching schedule window, -1 = always-on
    QString reason;             // "Outside schedule", "Disabled", ...
};

// Per-application bandwidth limiter.
//
// Built on the same WinDivert driver the system-wide limiter already uses. The
// design is forced by one documented constraint: ProcessId is NOT available at the
// network layer, which is the only layer where packets can be delayed. So the
// engine joins two layers:
//
//   * WINDIVERT_LAYER_FLOW     - carries ProcessId plus the flow's 5-tuple.
//   * WINDIVERT_LAYER_NETWORK  - where packets are actually held and re-injected.
//
// Attribution is continuous rather than one-shot, so an application that starts (or
// restarts) after Flowbit is picked up when it next opens a connection, and its
// tuples are re-registered automatically.
//
// Everything that can be decided without the driver - which rule applies to a path,
// which window is open, what the tuple set means - is kept out of the driver path so
// it can be unit-tested. See tools/test_applimit.cpp.
class AppLimitEngine : public QObject
{
    Q_OBJECT

public:
    explicit AppLimitEngine(QObject *parent = nullptr);

private slots:
    // Recomputes which rules are active (schedules) and re-applies the
    // per-rule rates. Runs every second while the engine is running, so
    // a window boundary is honoured within about a second of wall time.
    void onScheduleTick();

private:
    QTimer *m_scheduleTick = nullptr;

public:
    ~AppLimitEngine() override;

    // Replaces the set of application rules. Safe to call repeatedly; it performs a
    // differential update so an unchanged rule does not disturb its token bucket.
    void setRules(const QVector<AppRule> &rules);

    // Master switch for the whole feature, independent of the system-wide limiter.
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled.load(); }

    // Recomputes which rules are active now and pushes their rates into the
    // buckets. Schedule boundaries are wall-clock events, so this is called on a
    // timer as well as on every settings change. Does nothing when disabled.
    void refresh();

    // True once the flow and network handles are open.
    bool isRunning() const { return m_running.load(); }

    // Reason the engine is not running, or empty. Distinguishes "not elevated" from
    // "no rules" so the UI never shows a silent zero.
    QString errorString() const;

    // Non-fatal note about how enforcement is running, or empty. Currently used
    // when the driver's flow layer is unavailable on this Windows build and
    // attribution falls back to connection-table polling.
    QString attributionNote() const;

    // Current limits, for the UI and for tests.
    QVector<AppLimitState> states() const;

    // Applications currently matched to a running process, with their live rates.
    // Used by the page to show whether a rule is actually doing anything.
    struct LiveApp {
        quint64 ruleId = 0;
        double downBytesPerSec = 0.0;
        double upBytesPerSec = 0.0;
        int processCount = 0;
    };
    QVector<LiveApp> liveApps() const;

    // Packets paced since start, per direction. Reported so the UI can show that
    // the limiter is genuinely on the packet path rather than merely configured.
    quint64 pacedPacketsDown() const { return m_pacedDown.load(); }
    quint64 pacedPacketsUp() const { return m_pacedUp.load(); }

    // Packets lost because re-injection failed even after the retry budget. Reported in
    // diagnostics so "it drops my connection" can be answered with a number.
    quint64 injectionFailures() const { return m_injectFailures.load(); }

    // Token buckets for one rule, one per direction. Declared here because the
    // worker lambdas in the .cpp name the type; not part of the public API.
    struct Buckets;

    // ---- pure helpers, unit-tested without any driver ----------------------

    // Normalises a flow observation into the tuple form used for matching.
    static FlowTuple normaliseTuple(quint32 saddr, quint16 sport, quint32 daddr,
                                    quint16 dport, quint8 protocol, bool outbound);

    // IPv6 variant: the four address words are the raw UINT32s from the packet
    // header / flow event, kept in their original order.
    static FlowTuple normaliseV6Tuple(const quint32 saddr[4], quint16 sport,
                                      const quint32 daddr[4], quint16 dport,
                                      quint8 protocol, bool outbound);

    // Picks the rule that governs `path` (or `exe` when the path is unknown).
    // Returns the index into `rules`, or -1.
    //
    // Matching prefers the full path and only falls back to the executable name
    // when the rule has no path, so two unrelated binaries sharing a name such as
    // "launcher.exe" are not conflated.
    static int matchRuleByPath(const QVector<AppRule> &rules, const QString &path,
                               const QString &exe);

    // Resolves which of a rule's schedule windows is open at `now`, or -1 when the
    // rule is always-on (no windows) or no window matches.
    // `outAlwaysOn` distinguishes "no windows configured" from "outside every
    // window", which the UI reports differently.
    static int activeWindow(const AppRule &rule, const QDateTime &now, bool *outAlwaysOn);

    // The effective limit for a rule at `now`, accounting for enabled state and
    // schedule. `active` is false when the rule should not limit right now.
    static AppLimitState evaluate(const AppRule &rule, const QDateTime &now);

signals:
    void stateChanged();
    void errorOccurred(const QString &message);

private:
    // Copies the active rules' rates into the token buckets. Separate from
    // refresh() so it can be reasoned about without touching the schedule.
    void applyBucketsFromStates();

    class Impl;
    std::unique_ptr<Impl> m_impl;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_running{false};
    std::atomic<quint64> m_pacedDown{0};
    std::atomic<quint64> m_pacedUp{0};

    // Packets whose re-injection failed and were therefore LOST.
    //
    // WinDivertSend is the second half of a recv/send pair: a failed send means that packet never
    // reaches the stack. The shapes of failure differ - the injection queue fills while the driver
    // is busy - so this is counted and retried briefly rather than dropped in silence, which is
    // what a shaped connection "randomly stalling" is made of.
    std::atomic<quint64> m_injectFailures{0};
};

} // namespace net
