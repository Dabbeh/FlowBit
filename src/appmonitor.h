#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <chrono>
#include <memory>

namespace net {

// One application's activity over the last sampling interval.
struct AppTraffic {
    // Display identity. The path is kept as well as the name so the icon can be
    // extracted and cached without re-resolving the process.
    QString executablePath;   // full path, empty when unknown
    QString executableName;   // e.g. "chrome.exe"
    QString displayName;      // friendly name when known, else the executable name
    QVector<quint32> pids;    // every process grouped into this application

    // Raw byte counts for the interval, so rates are derived rather than stored.
    quint64 bytesIn = 0;
    quint64 bytesOut = 0;

    // Bytes per second, computed from the interval length.
    double downBytesPerSec = 0.0;
    double upBytesPerSec = 0.0;

    double totalBytesPerSec() const { return downBytesPerSec + upBytesPerSec; }
};

// Whether per-process monitoring is running, and why not when it is not.
enum class MonitorState {
    Stopped,      // never started, or explicitly stopped
    Running,      // a trace session is live and events are arriving
    Unavailable,  // could not start: not elevated, provider missing, session busy
};

// Live per-application network monitoring.
//
// Windows sources, in the order the implementation tries them:
//
//   1. ETW, the Microsoft-Windows-Kernel-Network provider. Its TCP/UDP events
//      carry the PID and the byte count for each operation, which is the only
//      Windows mechanism that attributes traffic to a process accurately. It
//      requires Administrator rights - which Flowbit already holds, because its
//      manifest requests requireAdministrator for WinDivert.
//
//   2. Nothing else. The per-connection TCP EStats API looks promising but
//      SetPerTcpConnectionEStats returns ERROR_ACCESS_DENIED without elevation,
//      and GetPerTcpConnectionEStats then reports success while writing nothing -
//      so its output cannot be trusted and it is deliberately not used. See
//      docs/app-monitoring-findings.md for the probe results.
//
// Everything runs on its own thread. The UI polls snapshots; it never touches
// the trace session.
class AppMonitor : public QObject
{
    Q_OBJECT

public:
    explicit AppMonitor(QObject *parent = nullptr);
    ~AppMonitor() override;

    // Starts the trace session. Safe to call repeatedly; a call while running is
    // a no-op. Returns false and sets errorString() when the session cannot be
    // created, which the UI is expected to surface rather than show zeroes.
    //
    // Synchronised on purpose: tests need the state settled before they assert.
    // The UI should prefer the asynchronous variant below so a wedged session
    // can never hold the interface for the length of a bounded wait.
    bool start();

    // Asynchronous start: performs exactly what start() does, but on a worker
    // thread, reporting the outcome through stateChanged(). Safe to call while
    // already starting (the extra request is coalesced).
    void startAsync();

    // Advances the measurement interval: swaps the ingested byte maps into a
    // finished snapshot at per-second rates and emits updated(). Call this on
    // a ~1-second cadence from the thread that owns this object (the dashboard
    // refresh timer already does). Without ticks the snapshot never fills -
    // that exact omission is why the panel once stayed empty forever despite a
    // running, ingesting session.
    void tick();

    void stop();

    // Same teardown as stop(), on a worker thread.
    //
    // stop() can spin for up to a second waiting on an in-flight start and then stops the trace
    // session. The activity panel calls it every time it collapses, from a deferred lambda on the
    // GUI thread - so collapsing the panel could freeze the window. Nothing about the teardown
    // needs the GUI thread.
    void stopAsync();

    MonitorState state() const { return m_state.load(); }
    bool isRunning() const { return state() == MonitorState::Running; }

    // True between startAsync() and the worker finishing its session setup -
    // the panel uses this to show a "Starting" state instead of prematurely
    // claiming "nothing is using the network".
    bool isStarting() const { return m_starting.load(); }

    // Seconds of monitoring in this session (one tick per second, reset on start).
    //
    // Needed by diagnostics to tell "the source is broken" from "the panel was never opened" and
    // from "nothing has transferred yet" - three states that all read as zero events.
    int runningSeconds() const { return m_tickCount; }

    // Human-readable reason for the current Unavailable state, or empty.
    QString errorString() const;

    // ETW counters. seen = events arriving in the callback; decoded = the subset
    // that parsed to a usable pid+size. The pair separates "the session/provider
    // is delivering nothing" (both 0) from "events arrive but the payload schema
    // on this Windows build does not match our decoder" (seen > 0, decoded == 0),
    // which are entirely different fixes.
    quint64 eventsSeenTotal() const;
    quint64 eventsDecodedTotal() const;

    // UDP events counted in this session. Zero while ETW is the source and UDP traffic has
    // flowed means the protocol is not arriving, which is worth being able to see.
    quint64 udpEventsTotal() const;

    // True while the panel is being fed from the TCP EStats fallback instead of
    // ETW (some Windows builds deliver zero kernel-network events). TCP-only is
    // the trade: QUIC/UDP traffic is not attributed until ETW recovers, so the
    // UI says so rather than quietly under-reporting.
    bool usingTcpFallback() const;

    // Why the reduced-capability source is in use, or empty when the full ETW
    // source is running. Surfaced as a scope note so the panel explains itself with
    // the real cause instead of a generic sentence - and so a blocked source never
    // reads as a failure.
    QString fallbackReason() const;

    // Per-event-id ETW schema and decode counts, for the diagnostics report.
    // One line per event id seen, naming every payload property and its type.
    QString etwEventLayout() const;

    // Raw samples of events whose payload addresses did not decide
    // direction. Byte order is settled from these, not from theory.
    QString addressMissReport() const;

    // The first few REAL (non-loopback) events with every field and the direction
    // decision. This is what settles the send/receive mapping from evidence.
    QString rawEventReport() const;

    // Phase 0.1: raw hex addresses, both byte-order interpretations, ports, size
    // and the local address list. No interpretation - this is the evidence used
    // to settle the direction rule.
    QString rawBitsReport() const;

    // True when the process token carries Administrator rights, which is the
    // precondition for ETW. Used to produce a precise diagnostic rather than a
    // generic failure.
    static bool isElevated();

    // The ETW session name Flowbit uses, exposed so diagnostics and the manual
    // stop command ("logman stop <name>") can refer to the same session.
    static QString sessionName();

    // True when a trace session of that name is left behind - a previous run was
    // closed without releasing it. Read-only query; starts and stops nothing.
    static bool leftoverSessionExists();

    // Measures which data sources work on THIS machine (ETW session creation, TCP
    // EStats, connection table) and reports each with its raw result. Diagnosis by
    // measurement: the feature was misdiagnosed repeatedly by inferring a cause
    // from a single error code.
    static QString dataSourceReport();

    // Issues a ControlTrace stop for the session from THIS process. Called by the
    // --clear-monitor-session self-execution path, which AppMonitor spawns from
    // its already elevated parent when logman cannot see a kernel ghost. This
    // function never touches the UI, so even a stuck kernel lock can only block
    // the short-lived child - the parent bounds the wait.
    static void clearGhostSession();

    // Applications seen during the most recent completed interval, sorted by
    // total bytes per second, highest first. Safe to call from the UI thread.
    QVector<AppTraffic> snapshot() const;

    // Traffic the session could not attribute to a process still running - for
    // example a service that has since exited. Reported separately rather than
    // assigned to an application, because guessing would be misleading.
    AppTraffic unattributed() const;

    // Interval between rate computations, in milliseconds.
    void setIntervalMs(int ms);
    int intervalMs() const { return m_intervalMs; }

    // Test seam: declares the monitor Running without a trace session, so the
    // panel's presentation logic can be exercised on a machine where the session
    // cannot start. Never call this from application code.
    void forceRunningForTesting();

    // Test seam: feeds the aggregator directly so rate maths, grouping and
    // ordering can be verified without a live trace session.
    void injectForTesting(quint32 pid, const QString &exe, quint64 bytesIn, quint64 bytesOut);
    // Completes an interval using the injected data.
    void flushForTesting(double seconds);

signals:
    // Emitted once per interval, from the monitor thread.
    void updated();
    void stateChanged();

private:
    // Defined in the .cpp. Declared public so the file-local ETW callback can
    // name it; it is never exposed through this header's API.
public:
    class Impl;

private:
    std::unique_ptr<Impl> m_impl;

    std::atomic<MonitorState> m_state{MonitorState::Stopped};
    std::atomic<bool> m_starting{false};   // startAsync worker in flight
    std::atomic<bool> m_stopping{false};   // stopAsync worker in flight
    // Interval bookkeeping for tick()-driven snapshots (owned by the GUI thread).
    std::chrono::steady_clock::time_point m_lastTick{};
    int m_tickCount = 0;
    int m_intervalMs = 1000;
};

} // namespace net
