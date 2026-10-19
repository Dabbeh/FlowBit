#pragma once

#include "settings.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <thread>

namespace net {

// Token bucket that turns "bytes per second" into "how long must this packet
// wait". Tokens refill continuously from a steady clock, so a rate change or a
// stop request is picked up within one sleep slice.
//
// Exposed in the header (rather than hidden in the .cpp) so the pacing maths can
// be unit-tested without touching the network - see tools/test_pacer.cpp.
class Pacer
{
public:
    static constexpr qint64 kMinBurstBytes = 64 * 1024;
    static constexpr int kSleepSliceMs = 5;
    static constexpr int kMaxSleepSliceMs = 25;

    explicit Pacer(qint64 rateBps) { setRate(rateBps); }

    void setRate(qint64 rateBps)
    {
        if (rateBps == m_rate)
            return;
        m_rate = rateBps;
        m_burst = std::max<qint64>(kMinBurstBytes, m_rate / 4);
        m_tokens = std::min(m_tokens, m_burst);
        if (m_tokens < 0)
            m_tokens = 0;
    }

    qint64 rate() const { return m_rate; }

    // Blocks until the packet may be released. Returns false if `stopFlag` was
    // raised while waiting (the caller should still forward the packet).
    bool waitFor(size_t packetLen, const std::atomic<bool> &stopFlag,
                 qint64 *currentRate = nullptr)
    {
        if (m_rate <= 0) {
            if (currentRate)
                *currentRate = m_rate;
            return true;
        }

        if (m_last.time_since_epoch().count() == 0)
            m_last = Clock::now();

        refill();

        if (static_cast<qint64>(packetLen) <= m_tokens) {
            m_tokens -= static_cast<qint64>(packetLen);
            if (currentRate)
                *currentRate = m_rate;
            return true;
        }

        // Not enough credit: wait only as long as the current deficit requires.
        // The loop re-checks the live rate every slice so a change takes effect
        // immediately instead of after the whole (possibly seconds-long) wait.
        while (!stopFlag.load(std::memory_order_relaxed)) {
            if (m_rate <= 0) {
                // Became unlimited mid-wait: release immediately.
                if (currentRate)
                    *currentRate = 0;
                return true;
            }
            if (static_cast<qint64>(packetLen) <= m_tokens)
                break;

            const qint64 deficit = static_cast<qint64>(packetLen) - m_tokens;
            qint64 waitMs = (deficit * 1000) / m_rate;
            if (waitMs < kSleepSliceMs)
                waitMs = kSleepSliceMs;
            waitMs = std::min<qint64>(waitMs, kMaxSleepSliceMs);

            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            refill();
        }

        if (static_cast<qint64>(packetLen) <= m_tokens)
            m_tokens -= static_cast<qint64>(packetLen);
        else
            m_tokens = 0;  // stop requested: let the packet through anyway

        if (currentRate)
            *currentRate = m_rate;
        return !stopFlag.load(std::memory_order_relaxed);
    }

private:
    using Clock = std::chrono::steady_clock;

    void refill()
    {
        const auto now = Clock::now();
        if (m_rate > 0 && m_last.time_since_epoch().count() != 0) {
            const auto elapsedUs =
                std::chrono::duration_cast<std::chrono::microseconds>(now - m_last).count();
            m_tokens += (m_rate * elapsedUs) / 1000000;
            if (m_tokens > m_burst)
                m_tokens = m_burst;
        }
        m_last = now;
    }

    qint64 m_rate = 0;
    qint64 m_burst = kMinBurstBytes;
    qint64 m_tokens = 0;
    Clock::time_point m_last{};
};

// System-wide bandwidth limiter built on WinDivert.
//
// One worker thread per direction ("inbound" / "outbound") diverts matching
// packets to user mode and re-injects them through a token bucket, so the
// sender's TCP stack self-clocks down to the configured rate.
//
// A direction with a 0 (unlimited) rate keeps its WinDivert handle *closed*:
// while unlimited the engine is entirely out of the packet path, adding no
// latency and no CPU cost. Switching a direction back to unlimited closes the
// handle, and WinDivert automatically re-injects anything still queued.
//
// All public methods must be called from the thread that owns the engine
// (normally the GUI thread); workers are plain std::thread and report back
// through queued signals.
class ThrottleEngine : public QObject
{
    Q_OBJECT

public:
    explicit ThrottleEngine(QObject *parent = nullptr);
    ~ThrottleEngine() override;

    // Applies new per-direction rates in BYTES PER SECOND (0 = uncapped).
    // Starts, stops, or leaves each direction's worker running as needed.
    //
    // Bytes/sec is the app-wide canonical unit (see settings.h), so no
    // conversion happens here and a caller cannot accidentally pass KB/s.
    // What the driver service looked like when startup checked it.
    enum class DriverReadiness {
        Ready,          // startable; nothing needed doing
        Repaired,       // it had been left disabled, and the start type was restored
        Held,           // another instance is holding it; it will be fine once that exits
        NeedsRestart,   // marked for deletion; only a reboot clears that
        Unavailable,    // no service, or one belonging to a different application
    };

    // Called once at startup, before the first apply.
    //
    // The disabled-and-loaded state (START_TYPE 4, STATE 4) is invisible to an open that
    // succeeds against the already-loaded driver, and only bites at the NEXT load - after a
    // reboot, or when the last handle closes. Repairing during launch turns a visible
    // failure-then-recovery into a brief delay with nothing shown.
    //
    // Cheap: one registry read and, only when needed, one service config change. Safe to
    // call from the UI thread, and it does not touch the driver when it is already usable.
    // Brings the driver into a usable state: registers a missing service, repairs a disabled
    // one, and clears a pending-deletion deadlock where the machine allows it.
    //
    // Static, because it is a property of the installation rather than of an engine instance -
    // it touches no member state. That is what lets the application-limiter, which owns its own
    // driver handles, call it before deciding a limit cannot be enforced.
    static DriverReadiness ensureDriverReady();

    // Makes WinDivert register its own driver service, and reports whether it worked.
    //
    // The service is created by WinDivertOpen, the first time the driver is opened - measured,
    // not assumed. Flowbit only opens the driver when a limit is set, so on a fresh install
    // nothing ever registers it and every limit fails until the user happens to know to run the
    // application elevated from a folder holding both driver files.
    //
    // Opening with a filter that matches nothing makes the library register itself without
    // intercepting a single packet. The handle is closed immediately.
    //
    // Static, because it has nothing to do with an instance: it is a property of the
    // installation, and the installer calls it through a command-line switch with no engine
    // running at all.
    //
    // Needs administrator rights. Returns false without them, which is the honest answer rather
    // than a failure the caller has to interpret.
    static bool registerDriverService();

    void apply(RateBps downBps, RateBps upBps);

    // Detaches from the network entirely (all directions uncapped).
    void detach();

    bool isThrottling() const;
    RateBps downBps() const { return m_downBps.load(); }
    RateBps upBps() const { return m_upBps.load(); }

    // Cumulative counters, useful for a sanity check that packets really flow.
    quint64 downPackets() const { return m_downPackets.load(); }
    quint64 upPackets() const { return m_upPackets.load(); }

signals:
    void errorOccurred(const QString &message);
    void stateChanged(bool throttling);

private:
    struct Direction {
        const char *filter = nullptr;
        const char *label = nullptr;

        std::thread worker;
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> finished{false};    // worker returned; handle is gone
        std::atomic<qint64> rateBps{0};       // 0 = unlimited
        std::atomic<quint64> packets{0};
        std::atomic<quint64> bytes{0};
        std::atomic<void *> handle{nullptr};  // HANDLE, owned by the worker

        bool running() const { return worker.joinable(); }
    };

    void reconfigure(Direction &dir);
    void startWorker(Direction &dir);
    void stopWorker(Direction &dir);
    // Returns the worker's published WinDivert handle, or nullptr if it never
    // appeared (still starting up, or the driver open failed).
    void *waitForHandle(Direction &dir);
    void workerMain(Direction &dir);

    Direction m_in;
    Direction m_out;
    std::atomic<RateBps> m_downBps{0};
    std::atomic<RateBps> m_upBps{0};
    std::atomic<bool> m_throttling{false};
    std::atomic<quint64> m_downPackets{0};
    std::atomic<quint64> m_upPackets{0};
};

} // namespace net
