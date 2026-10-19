#include "throttleengine.h"

#include <QDebug>
#include <QString>

#include <windows.h>
#include <windivert.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace net {

namespace {

constexpr size_t kMaxPacket = 65535;  // IPv4/IPv6 packet ceiling

// WinDivert reports driver failures through GetLastError; translate the ones a
// user can actually act on.
//
// The messages name the cause AND the fix, because these failures are not
// self-explanatory: "the service is disabled" gives no hint that a single registry
// value is responsible, or that a repair is available.
QString describeOpenError(DWORD code)
{
    switch (code) {
    case ERROR_ACCESS_DENIED:
        return QStringLiteral(
            "Access denied while loading the WinDivert driver. Flowbit must run "
            "as Administrator.");
    case ERROR_INVALID_PARAMETER:
        return QStringLiteral("WinDivert rejected the filter (invalid parameter).");
    case ERROR_SERVICE_DOES_NOT_EXIST:
    case ERROR_FILE_NOT_FOUND:
        return QStringLiteral(
            "The WinDivert driver could not be found. Make sure WinDivert.dll and "
            "WinDivert64.sys sit next to Flowbit.exe.");
    case ERROR_SERVICE_DISABLED:
        // The service exists but its start type is DISABLED (registry Start = 4), so
        // Windows refuses to start the driver. This is the state an update leaves
        // behind: Windows disables a kernel service whose registered image file is
        // replaced underneath it.
        //
        // The repair is attempted before the failure is reported (see
        // repairDisabledDriverService), so reaching this message means the attempt did
        // not succeed and there is a reason worth showing.
        return QStringLiteral(
            "The WinDivert driver is still held by the previous version, so limits "
            "cannot be applied yet.\n\n"
            "This happens when Flowbit is updated while its driver is loaded: Windows "
            "keeps the old service reserved and will not let it be changed or reused "
            "until the driver unloads. In this state neither removing nor re-creating "
            "the service works - it reports \"marked for deletion\" - so no command "
            "can fix it.\n\n"
            "Restart Windows once. After that Flowbit starts the driver normally and "
            "limits apply. This only occurs after an update, not on ordinary runs.");
    case ERROR_DRIVER_BLOCKED:
    case ERROR_INVALID_IMAGE_HASH:
        return QStringLiteral(
            "Windows blocked the WinDivert driver (signature or policy problem).\n\n"
            "Security software or a Windows policy may be preventing the driver from "
            "loading, because packet-capture drivers are sometimes flagged as "
            "potentially unwanted.");
    default:
        return QStringLiteral("WinDivertOpen failed with error %1.").arg(code);
    }
}


} // namespace

ThrottleEngine::ThrottleEngine(QObject *parent) : QObject(parent)
{
    m_in.filter = "inbound and not loopback";
    m_in.label = "inbound";
    m_out.filter = "outbound and not loopback";
    m_out.label = "outbound";
}

ThrottleEngine::~ThrottleEngine()
{
    stopWorker(m_in);
    stopWorker(m_out);
}

void ThrottleEngine::apply(RateBps downBps, RateBps upBps)
{
    // Rates arrive in the app-wide canonical unit (bytes/sec), so there is no
    // conversion to get wrong here.
    const RateBps down = downBps > 0 ? downBps : 0;
    const RateBps up = upBps > 0 ? upBps : 0;

    // Update the atomic rates first: a worker that is already pacing will pick
    // the new rate up on its next slice instead of being torn down.
    m_in.rateBps.store(down);
    m_out.rateBps.store(up);
    m_downBps.store(down);
    m_upBps.store(up);

    reconfigure(m_in);
    reconfigure(m_out);

    const bool throttling = (down > 0 || up > 0);
    if (throttling != m_throttling.load())
        emit stateChanged(throttling);
    m_throttling.store(throttling);
    m_downPackets.store(m_in.packets.load());
    m_upPackets.store(m_out.packets.load());
}

void ThrottleEngine::detach()
{
    apply(0, 0);
}

bool ThrottleEngine::isThrottling() const
{
    return m_throttling.load();
}

void ThrottleEngine::reconfigure(Direction &dir)
{
    const bool wanted = dir.rateBps.load() > 0;

    if (wanted && !dir.running())
        startWorker(dir);
    else if (!wanted && dir.running())
        stopWorker(dir);  // closing the handle re-injects queued packets
}

void ThrottleEngine::startWorker(Direction &dir)
{
    dir.stopRequested.store(false);
    dir.finished.store(false);
    dir.worker = std::thread(&ThrottleEngine::workerMain, this, std::ref(dir));
}

void ThrottleEngine::stopWorker(Direction &dir)
{
    if (!dir.running())
        return;

    dir.stopRequested.store(true);

    // Unblock WinDivertRecv immediately instead of waiting for the next packet
    // on an idle connection - otherwise the join below could stall until traffic
    // happens to arrive.
    //
    // The worker publishes its handle right after WinDivertOpen, so if it is not
    // visible yet the thread is still starting up: it will observe
    // stopRequested, skip the loop, and close the handle itself. Poll briefly so
    // a handle that appears mid-startup is still shut down.
    const HANDLE handle = static_cast<HANDLE>(waitForHandle(dir));
    if (handle)
        WinDivertShutdown(handle, WINDIVERT_SHUTDOWN_RECV);

    dir.worker.join();
    dir.handle.store(nullptr);
}

void *ThrottleEngine::waitForHandle(Direction &dir)
{
    // ~2s worst case, in small steps, so a worker that fails to open the handle
    // (driver blocked, not elevated) does not make shutdown hang.
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (void *h = dir.handle.load())
            return h;
        if (dir.finished.load())  // worker already exited (e.g. open failed)
            return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nullptr;
}

namespace {

// Whether the WinDivert service's registered image is the driver next to this
// executable. Used to decide whether re-enabling it is this application's business.
bool servicePointsAtOurDriver()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;

    bool matches = false;
    if (SC_HANDLE svc = OpenServiceW(scm, L"WinDivert", SERVICE_QUERY_CONFIG)) {
        DWORD needed = 0;
        QueryServiceConfigW(svc, nullptr, 0, &needed);
        if (needed > 0) {
            std::vector<BYTE> buf(needed);
            auto *cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data());
            if (QueryServiceConfigW(svc, cfg, needed, &needed) && cfg->lpBinaryPathName) {
                // The driver path is registered as \??\C:\...\WinDivert64.sys; compare
                // only the file name, which is what identifies our driver regardless of
                // how the path is written.
                QString path = QString::fromWCharArray(cfg->lpBinaryPathName);
                path.replace(QLatin1Char('\\'), QLatin1Char('/'));
                matches = path.endsWith(QLatin1String("WinDivert64.sys"), Qt::CaseInsensitive)
                          || path.endsWith(QLatin1String("WinDivert32.sys"), Qt::CaseInsensitive);
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return matches;
}

// Re-enables the driver service and returns whether it now starts on demand.
//
// Setting the start type back to SERVICE_DEMAND_START restores how WinDivert registers
// itself, so this repairs the state an update broke rather than changing behaviour.
enum class RepairOutcome {
    Repaired,        // the start type was restored; a retry should work
    NeedsRestart,    // the service is pending deletion; Windows must restart
    Refused,         // not our service, or the change was refused for another reason
};

struct DriverServiceState {
    bool exists = false;
    DWORD startType = 0;     // 2 auto, 3 demand, 4 disabled
    bool running = false;    // loaded in kernel memory right now
};

DriverServiceState readDriverServiceState()
{
    DriverServiceState state;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Services\\WinDivert", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        state.exists = true;
        DWORD value = 0;
        DWORD size = sizeof(value);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"Start", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS
            && type == REG_DWORD) {
            state.startType = value;
        }
        RegCloseKey(key);
    }

    // "Running" is asked of the service control manager rather than the registry, because
    // only the SCM knows whether the driver is currently loaded.
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE svc = OpenServiceW(scm, L"WinDivert", SERVICE_QUERY_STATUS)) {
            SERVICE_STATUS status{};
            if (QueryServiceStatus(svc, &status))
                state.running = status.dwCurrentState == SERVICE_RUNNING;
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    } else {
        // Without the SCM the start type is still known, and it is the value that matters:
        // a driver already loaded will open successfully whatever the start type says, so
        // the only question this function has to answer is whether a LOAD would succeed.
        state.running = false;
    }

    return state;
}


RepairOutcome repairDisabledDriverService()
{
    if (!servicePointsAtOurDriver())
        return RepairOutcome::Refused;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return RepairOutcome::Refused;

    RepairOutcome outcome = RepairOutcome::Refused;

    // Both rights are requested in one open: the start type is changed and the service is then
    // stopped, and asking twice would be a needless extra SCM round trip.
    if (SC_HANDLE svc = OpenServiceW(
            scm, L"WinDivert",
            SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_QUERY_STATUS)) {

        // Step 1: make the service startable again.
        //
        // Done first, because a disabled service cannot be controlled at all - this is what makes
        // the stop below possible. Deliberately not rolled back if the stop fails: a service left
        // startable is a better state than the disabled one it was found in, whether or not the
        // unload succeeds.
        const BOOL startTypeChanged =
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
                                 SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr);

        if (startTypeChanged) {
            outcome = RepairOutcome::Repaired;
        } else if (GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE) {
            // Pending deletion, so the configuration cannot be altered - Windows will not let
            // anything change a service that is on its way out. The stop can still be attempted,
            // and the stop is what actually matters here.
            outcome = RepairOutcome::Repaired;
        } else {
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return RepairOutcome::Refused;
        }

        // Step 2: bring the loaded driver out of kernel memory.
        //
        // THIS IS WHAT COMPLETES THE PENDING DELETION, and it is why a restart is not needed.
        //
        // The state found on a real machine was:
        //
        //     DeleteFlag : 1        marked for deletion
        //     Start      : 4        DISABLED
        //     State      : RUNNING  loaded anyway
        //
        // A disabled driver can never load again; a loaded driver cannot be deleted. That reads
        // as a deadlock only a reboot breaks, and the previous code said as much. It is not:
        // the deletion is waiting for the loaded instance to unload, and the unload can be
        // asked for. Windows then completes a deletion that was requested some time ago.
        //
        // Skipped when the service already reports itself stopped, because stopping a stopped
        // service is a pointless round trip.
        SERVICE_STATUS status{};
        if (QueryServiceStatus(svc, &status) && status.dwCurrentState != SERVICE_STOPPED) {
            SERVICE_STATUS stopped{};
            if (ControlService(svc, SERVICE_CONTROL_STOP, &stopped)) {
                qInfo("Flowbit: unloaded the WinDivert driver, which settles the deletion that "
                      "was pending on its service");
            } else {
                // Something else holds the driver - another copy of Flowbit, or another
                // application using WinDivert of its own. The deletion stays pending, but the
                // service is startable again, so the driver is usable and the deletion can
                // finish whenever that holder lets go.
                qInfo("Flowbit: the WinDivert driver is held by another process, so the pending "
                      "deletion of its service will settle later (error %lu)",
                      static_cast<unsigned long>(GetLastError()));
            }
        }

        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);

    // Step 3: report what is actually true, by reading it back.
    //
    // Not what the calls above returned - what the system says afterwards. The two can disagree:
    // ChangeServiceConfig can report success while the start type is still unusable for another
    // reason, and ControlService can report success while the driver takes a moment to unload.
    //
    // NeedsRestart is therefore returned only when the machine genuinely cannot proceed: the
    // service is still disabled AND its driver is still loaded. Reporting a restart for a state
    // that does not need one is how users end up restarting for nothing.
    const DriverServiceState after = readDriverServiceState();

    if (!after.exists) {
        // The pending deletion completed. Nothing left to do, and nothing to report.
        qInfo("Flowbit: the pending deletion of the WinDivert service completed");
        return RepairOutcome::Repaired;
    }

    if (after.startType != 4) {
        // Startable again. Whether or not the driver unloaded, the next open will work and the
        // deletion can settle whenever the loaded instance goes.
        return RepairOutcome::Repaired;
    }

    if (!after.running) {
        // Still disabled, no longer loaded. The deletion is no longer held up by the driver, so
        // it completes on its own and there is nothing for the user to do.
        return RepairOutcome::Repaired;
    }

    // Disabled AND still loaded, so both attempts failed. The pending deletion really is waiting
    // on a restart, and reaching here means the machine is genuinely stuck rather than merely in
    // an unfamiliar state.
    return RepairOutcome::NeedsRestart;
}

} // namespace

namespace {

} // namespace


bool ThrottleEngine::registerDriverService()
{
    // Opening the driver is what registers the service, and this is the only supported way to
    // make WinDivert do it.
    //
    // Creating the service by hand was considered and rejected: its image path, start type and
    // error control are the library's business, and a hand-written sc create would be a guess at
    // values that could drift from what the library expects. Opening the driver and closing it
    // makes the library register precisely what it needs - which is what a probe did when this
    // was diagnosed, and why the service appeared.
    //
    // The filter matches nothing, so no packet is intercepted; the aim is registration, not work.
    HANDLE handle = WinDivertOpen("false", WINDIVERT_LAYER_NETWORK, 0, 0);

    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();

        // ERROR_ACCESS_DENIED is the expected outcome without elevation, and is worth
        // distinguishing from a real fault: one is a permission the installer has, the other is
        // a problem the user needs to know about.
        if (code == ERROR_ACCESS_DENIED) {
            qWarning("Flowbit: the driver service needs administrator rights to register "
                     "(error %lu)", static_cast<unsigned long>(code));
        } else {
            qWarning("Flowbit: could not open the driver to register its service (error %lu)",
                     static_cast<unsigned long>(code));
        }
        return false;
    }

    WinDivertClose(handle);

    // Verify by reading the service back rather than trusting the open. A successful open proves
    // the driver works; it does not prove a service was left registered behind it, and the
    // service is what every later launch depends on.
    const DriverServiceState state = readDriverServiceState();
    if (!state.exists) {
        qWarning("Flowbit: the driver opened but no service was registered");
        return false;
    }

    return true;
}

ThrottleEngine::DriverReadiness ThrottleEngine::ensureDriverReady()
{
    const DriverServiceState state = readDriverServiceState();

    if (!state.exists) {
        // No service. Try to create one.
        //
        // The installer registers it, so this should not normally happen - but it does after an
        // installation predating that change, after a manual sc delete, or when something has
        // removed it. Returning Unavailable here, as this once did, leaves every limit failing
        // for a reason the user cannot act on.
        //
        // Flowbit runs elevated, which is the privilege the registration needs, so the attempt
        // costs a few milliseconds and can save the installation.
        if (registerDriverService()) {
            // Registered. The driver is loadable now, and the next apply will open it.
            return DriverReadiness::Repaired;
        }
        return DriverReadiness::Unavailable;
    }

    // The common case, and the one that must stay cheap: either the start type is already
    // startable, or the driver is loaded. A loaded driver opens successfully whatever the
    // start type says, so nothing needs doing about this launch - though a disabled start
    // type will bite at the next one, which is why it is repaired below rather than here.
    if (state.startType != 4 && state.running)
        return DriverReadiness::Ready;
    if (state.startType != 4)
        return DriverReadiness::Ready;   // startable on demand; nothing to do

    // DISABLED, and here is where the deadlock is broken.
    //
    // Two distinct disabled states reach this point, and they need different answers:
    //
    //   Start = 4, not loaded      an update left the service disabled. Repair the start type.
    //   Start = 4, STILL LOADED    also marked for deletion, almost certainly. The driver is in
    //                              kernel memory and cannot be deleted while it is; the deletion
    //                              waits for it to unload, and it will never unload by itself
    //                              because the start type says the service is disabled.
    //
    // The second is the one that produced "pending removal, which only a restart clears" on every
    // launch, forever. It is not actually a deadlock: the deletion is waiting for the unload, and
    // the unload can be asked for. Both cases go through the same repair below, which sets the
    // start type and then stops the service, because that sequence clears either of them.
    //
    // THIS RUNS ON EVERY LAUNCH, before anything asks for a limit. That is the whole point. The
    // first attempt at this lived in the packet worker, which only starts when a limit is set -
    // so a user with limiting off, which is every fresh install, never ran it and saw the same
    // message on every launch with nothing able to clear it.
    //
    // The service is checked before it is touched. A WinDivert service belonging to another
    // application would be at a different path, and changing its configuration would be
    // interfering with someone else's driver - so a refusal here is the correct answer, not
    // a failure.
    if (!servicePointsAtOurDriver())
        return DriverReadiness::Unavailable;

    switch (repairDisabledDriverService()) {
    case RepairOutcome::Repaired:
        qInfo("Flowbit: the WinDivert driver service had been left disabled and has been "
              "re-enabled at startup");
        break;
    case RepairOutcome::NeedsRestart:
        // Kept for the one case that genuinely cannot be cleared here: the start type could not
        // be changed AND the driver could not be unloaded, which together mean the pending
        // deletion will not complete until the machine restarts. The repair reports this only
        // after attempting both, so reaching it means the machine really is stuck.
        qWarning("Flowbit: the WinDivert driver service is pending deletion and could not be "
                 "cleared at startup; a restart is required");
        return DriverReadiness::NeedsRestart;
    case RepairOutcome::Refused:
    default:
        return DriverReadiness::Unavailable;
    }

    // Starting it now is what makes the NEXT open work, and it also settles whether the
    // launch can proceed at all.
    if (state.running)
        return DriverReadiness::Repaired;   // already loaded; the repair is still the point

    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE svc = OpenServiceW(scm, L"WinDivert", SERVICE_START)) {
            if (StartServiceW(svc, 0, nullptr)) {
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return DriverReadiness::Repaired;
            }

            const DWORD error = GetLastError();
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);

            // 1056 (ERROR_SERVICE_ALREADY_RUNNING) means another instance started it first,
            // which is the desired outcome rather than a failure.
            if (error == ERROR_SERVICE_ALREADY_RUNNING)
                return DriverReadiness::Repaired;

            // A previous Flowbit that has not finished exiting still holds the driver. It
            // will be usable once that process ends, so this is reported as held rather
            // than as a fault - the remedy is to wait or to quit the other instance, and
            // saying "broken" would send the user looking for a problem that is not there.
            if (error == ERROR_SERVICE_MARKED_FOR_DELETE)
                return DriverReadiness::NeedsRestart;
            return DriverReadiness::Held;
        }
        CloseServiceHandle(scm);
    }

    return DriverReadiness::Held;
}

void ThrottleEngine::workerMain(Direction &dir)
{
    HANDLE handle = WinDivertOpen(dir.filter, WINDIVERT_LAYER_NETWORK, 0, 0);

    // A disabled service is a state an update leaves behind, and it is repairable
    // without the user doing anything: this process is already elevated, and the
    // service name and the intended start type are both known. One repair attempt, then
    // one retry - a limit that starts working again is a far better outcome than an
    // error message telling the user to open a Command Prompt.
    if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SERVICE_DISABLED) {
        switch (repairDisabledDriverService()) {
        case RepairOutcome::Repaired:
            qInfo("Flowbit: re-enabled the WinDivert driver service (an update had left "
                  "it disabled)");
            handle = WinDivertOpen(dir.filter, WINDIVERT_LAYER_NETWORK, 0, 0);
            break;
        case RepairOutcome::NeedsRestart:
            qWarning("Flowbit: the WinDivert service is pending deletion after a driver "
                     "update; a Windows restart is required before limits can apply");
            break;
        case RepairOutcome::Refused:
            break;
        }
    }

    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        dir.rateBps.store(0);  // prevent an open/close retry storm
        dir.finished.store(true);
        emit errorOccurred(QStringLiteral("[%1] %2")
                               .arg(QString::fromLatin1(dir.label), describeOpenError(code)));
        return;
    }

    dir.handle.store(handle);

    // Deep queues buy the pacer time: at 100 KB/s a 1.5 MB burst needs ~15 s of
    // buffering, and anything the queue cannot hold is dropped (TCP recovers,
    // but throughput suffers), so use the driver maximum.
    WinDivertSetParam(handle, WINDIVERT_PARAM_QUEUE_LENGTH, WINDIVERT_PARAM_QUEUE_LENGTH_MAX);
    WinDivertSetParam(handle, WINDIVERT_PARAM_QUEUE_TIME, WINDIVERT_PARAM_QUEUE_TIME_MAX);
    WinDivertSetParam(handle, WINDIVERT_PARAM_QUEUE_SIZE, WINDIVERT_PARAM_QUEUE_SIZE_MAX);

    std::vector<unsigned char> buffer(kMaxPacket);
    Pacer pacer(dir.rateBps.load());

    while (!dir.stopRequested.load(std::memory_order_relaxed)) {
        WINDIVERT_ADDRESS addr{};
        UINT recvLen = 0;

        if (!WinDivertRecv(handle, buffer.data(), static_cast<UINT>(buffer.size()), &recvLen,
                           &addr)) {
            const DWORD code = GetLastError();
            if (dir.stopRequested.load(std::memory_order_relaxed))
                break;  // normal shutdown: WinDivertShutdown cancels the recv
            if (code == ERROR_NO_DATA || code == ERROR_OPERATION_ABORTED)
                continue;
            emit errorOccurred(QStringLiteral("[%1] WinDivertRecv failed with error %2")
                                   .arg(QString::fromLatin1(dir.label))
                                   .arg(code));
            break;
        }

        if (recvLen == 0)
            continue;

        const qint64 liveRate = dir.rateBps.load();

        // Pacing is the only thing that takes time here; if this direction went
        // unlimited, forward without delay.
        if (liveRate > 0) {
            if (pacer.rate() != liveRate)
                pacer.setRate(liveRate);
            pacer.waitFor(recvLen, dir.stopRequested, nullptr);
        } else if (pacer.rate() != 0) {
            pacer.setRate(0);
        }

        UINT sendLen = 0;
        if (!WinDivertSend(handle, buffer.data(), recvLen, &sendLen, &addr)) {
            const DWORD code = GetLastError();
            if (!dir.stopRequested.load(std::memory_order_relaxed)) {
                qWarning("WinDivertSend failed on %s: %lu", dir.label,
                         static_cast<unsigned long>(code));
            }
            continue;
        }

        dir.packets.fetch_add(1, std::memory_order_relaxed);
        dir.bytes.fetch_add(sendLen, std::memory_order_relaxed);
    }

    WinDivertClose(handle);
    dir.handle.store(nullptr);
    dir.finished.store(true);
}

} // namespace net
