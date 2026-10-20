#include "diagnostics.h"

#include "appmonitor.h"
#include "applimitengine.h"
#include "settingsstore.h"
#include "theme.h"
#include "units.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QOperatingSystemVersion>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

#include <windows.h>
#include <psapi.h>
#include <softpub.h>
#include <wintrust.h>

#include <vector>

namespace net {

namespace {

// ---------------------------------------------------------------------------
// Windows state readers
//
// All read-only. Nothing here changes the system, which is what makes the
// diagnostics safe to collect on any machine.
// ---------------------------------------------------------------------------

QString wideToQ(const wchar_t *s)
{
    return QString::fromWCharArray(s);
}

// Reads a REG_SZ / REG_EXPAND_SZ value, or an empty string.
QString readServiceString(HKEY root, const wchar_t *subkey, const wchar_t *name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};

    wchar_t buf[1024] = {};
    DWORD len = sizeof(buf);
    DWORD type = 0;
    QString result;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &len)
            == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        result = wideToQ(buf);
    }
    RegCloseKey(key);
    return result;
}

// Reads a REG_DWORD, or `fallback` when it is absent.
DWORD readServiceDword(HKEY root, const wchar_t *subkey, const wchar_t *name, DWORD fallback)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return fallback;

    DWORD value = 0;
    DWORD len = sizeof(value);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, name, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&value), &len)
                        == ERROR_SUCCESS
                    && type == REG_DWORD;
    RegCloseKey(key);
    return ok ? value : fallback;
}

struct ServiceState {
    bool present = false;
    DWORD startType = 0;      // 2 auto, 3 demand, 4 disabled
    bool running = false;
    QString imagePath;
};

ServiceState queryWinDivertService()
{
    ServiceState s;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return s;

    SC_HANDLE svc = OpenServiceW(scm, L"WinDivert", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    if (svc) {
        s.present = true;

        DWORD needed = 0;
        QueryServiceConfigW(svc, nullptr, 0, &needed);
        if (needed > 0) {
            std::vector<BYTE> buf(needed);
            auto *cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data());
            if (QueryServiceConfigW(svc, cfg, needed, &needed)) {
                s.startType = cfg->dwStartType;
                s.imagePath = wideToQ(cfg->lpBinaryPathName);
            }
        }

        SERVICE_STATUS status{};
        if (QueryServiceStatus(svc, &status))
            s.running = status.dwCurrentState == SERVICE_RUNNING;

        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);

    // The registry value is authoritative for what Windows will do on next start,
    // so it is read too rather than inferred from the config query.
    if (s.present) {
        s.startType = readServiceDword(HKEY_LOCAL_MACHINE,
                                       L"SYSTEM\\CurrentControlSet\\Services\\WinDivert", L"Start",
                                       s.startType);
    }
    return s;
}

QString describeStartType(DWORD t)
{
    switch (t) {
    case 0:
        return QStringLiteral("boot");
    case 1:
        return QStringLiteral("system");
    case 2:
        return QStringLiteral("automatic");
    case 3:
        return QStringLiteral("demand (correct)");
    case 4:
        return QStringLiteral("DISABLED");
    default:
        return QStringLiteral("unknown (%1)").arg(t);
    }
}

// Whether the loaded WinDivert.dll can be found next to the executable, and what
// signature the driver file carries. The driver being from an unexpected signer is
// not an error, but it is a fact the user is entitled to see.
struct DriverFiles {
    bool dllPresent = false;
    bool sysPresent = false;
    QString dllPath;
    QString sysPath;
    QString signatureStatus;
    QString signer;
};

DriverFiles inspectDriverFiles()
{
    DriverFiles f;
    const QString dir = QCoreApplication::applicationDirPath();

    f.dllPath = QDir(dir).filePath(QStringLiteral("WinDivert.dll"));
    f.sysPath = QDir(dir).filePath(QStringLiteral("WinDivert64.sys"));
    f.dllPresent = QFileInfo::exists(f.dllPath);
    f.sysPresent = QFileInfo::exists(f.sysPath);

    if (f.sysPresent) {
        // Verify the signature through the Win32 API rather than shelling out, so
        // the report does not depend on PowerShell being present.
        const std::wstring native = QDir::toNativeSeparators(f.sysPath).toStdWString();
        LONG status = 0;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = native.c_str();
        WINTRUST_DATA data{};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = WTD_REVOKE_NONE;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &fileInfo;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

        const LONG rc = WinVerifyTrust(nullptr, &action, &data);
        if (rc == ERROR_SUCCESS) {
            status = 0;
            f.signatureStatus = QStringLiteral("valid");
        } else {
            status = rc;
            f.signatureStatus = QStringLiteral("NOT VERIFIED (0x%1)")
                                    .arg(quint32(rc), 8, 16, QLatin1Char('0'));
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &data);
        Q_UNUSED(status)
    }

    return f;
}

QString formatBytes(double bytes)
{
    if (bytes <= 0)
        return QStringLiteral("none");
    return formatBytes(double(bytes));
}

} // namespace

bool exportDiagnostics(const DiagnosticReport &report, const QString &path, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return false;
    }

    // Written through QSaveFile so a failure cannot leave a half-written report
    // where a complete one used to be.
    //
    // The device is opened in QIODevice::Text, which already translates "\n" to the
    // platform line ending on write. Converting here as well produced a doubled CR
    // ("\r\r\n") in the exported file, so the text is passed through exactly as
    // toText() built it and the device does the translation once.
    file.write(report.toText().toUtf8());

    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

QString diagnosticExportPath()
{
    // The desktop is used rather than a config directory because the purpose of the
    // export is to be found and attached to a message.
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString dir = desktop.isEmpty() ? QDir::homePath() : desktop;
    return QDir(dir).filePath(QStringLiteral("flowbit-diagnostics.txt"));
}

int DiagnosticReport::countOf(DiagnosticItem::Status s) const
{
    int n = 0;
    for (const DiagnosticItem &i : items) {
        if (i.status == s)
            ++n;
    }
    return n;
}

QString DiagnosticReport::toText() const
{
    QString out;
    QTextStream ts(&out);

    ts << "Flowbit diagnostics\n";
    ts << "====================\n\n";

    QString currentGroup;
    for (const DiagnosticItem &i : items) {
        if (i.group != currentGroup) {
            currentGroup = i.group;
            ts << "\n" << currentGroup.toUpper() << "\n";
            ts << QString(currentGroup.size() + 1, QLatin1Char('-')) << "\n";
        }

        const char *mark = " ";
        switch (i.status) {
        case DiagnosticItem::Status::Ok:
            mark = "OK";
            break;
        case DiagnosticItem::Status::Info:
            mark = "--";
            break;
        case DiagnosticItem::Status::Warning:
            mark = "!!";
            break;
        case DiagnosticItem::Status::Problem:
            mark = "XX";
            break;
        }

        ts << QStringLiteral("[%1] %2: %3\n").arg(QString::fromLatin1(mark), i.name, i.value);
        if (!i.detail.isEmpty()) {
            // Indented so a multi-line fix stays visually attached to its reading.
            const QStringList lines = i.detail.split(QLatin1Char('\n'));
            for (const QString &line : lines)
                ts << QStringLiteral("       %1\n").arg(line);
        }
    }

    ts << "\nSummary: "
       << QStringLiteral("%1 problem(s), %2 warning(s)")
              .arg(countOf(DiagnosticItem::Status::Problem))
              .arg(countOf(DiagnosticItem::Status::Warning))
       << "\n";
    ts << "Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";

    return out;
}

DiagnosticReport collectDiagnostics(bool isElevated, const QString &monitorError, quint64 eventsSeen, quint64 eventsDecoded, bool monitorRunning, int monitorSeconds, const QString &eventLayout, const QString &addressMiss,
                                    const QString &rawEvents,
                                    const QString &rawBits)
{
    DiagnosticReport r;
    const auto add = [&r](const QString &group, const QString &name, const QString &value,
                          DiagnosticItem::Status st, const QString &detail = QString()) {
        DiagnosticItem i;
        i.group = group;
        i.name = name;
        i.value = value;
        i.detail = detail;
        i.status = st;
        r.items.append(i);
    };

    // ---- build ------------------------------------------------------------
    add(QStringLiteral("Build"), QStringLiteral("Application"),
        QStringLiteral("%1 %2").arg(QCoreApplication::applicationName(),
                                    QCoreApplication::applicationVersion()),
        DiagnosticItem::Status::Info);

    add(QStringLiteral("Build"), QStringLiteral("Qt runtime"),
        QStringLiteral("%1").arg(QString::fromLatin1(qVersion())), DiagnosticItem::Status::Info);

    add(QStringLiteral("Build"), QStringLiteral("Operating system"),
        QStringLiteral("%1 %2 (build %3)")
            .arg(QSysInfo::productType(), QSysInfo::productVersion(),
                 QString::number(QOperatingSystemVersion::current().microVersion())),
        DiagnosticItem::Status::Info);

    add(QStringLiteral("Build"), QStringLiteral("Kernel"),
        QStringLiteral("%1, %2-bit").arg(QSysInfo::kernelVersion()).arg(QSysInfo::currentCpuArchitecture().contains(QStringLiteral("64")) ? 64 : 32),
        DiagnosticItem::Status::Info);

    {
        const QString dir = QCoreApplication::applicationDirPath();
        const bool onDesktop = dir.contains(QStringLiteral("Desktop"), Qt::CaseInsensitive);
        add(QStringLiteral("Build"), QStringLiteral("Install location"), dir,
            onDesktop ? DiagnosticItem::Status::Warning : DiagnosticItem::Status::Info,
            onDesktop ? QStringLiteral(
                            "Flowbit is running from the Desktop. The WinDivert driver "
                            "service registers the path of its driver file, so replacing "
                            "that file (which happens when the app is rebuilt or moved) "
                            "can leave the service unable to start. Installing Flowbit to "
                            "a stable location such as %ProgramFiles% avoids this.")
                      : QString());
    }

    // ---- permissions ------------------------------------------------------
    add(QStringLiteral("Permissions"), QStringLiteral("Elevation"),
        isElevated ? QStringLiteral("running as Administrator")
                   : QStringLiteral("NOT elevated"),
        isElevated ? DiagnosticItem::Status::Ok : DiagnosticItem::Status::Problem,
        isElevated ? QString()
                   : QStringLiteral(
                         "Flowbit loads a kernel driver to shape traffic, which requires "
                         "Administrator rights. Without it, no limit of any kind can be "
                         "applied. Close Flowbit and start it again, accepting the "
                         "elevation prompt."));

    // ---- driver files -----------------------------------------------------
    const DriverFiles files = inspectDriverFiles();

    add(QStringLiteral("Driver files"), QStringLiteral("WinDivert.dll"),
        files.dllPresent ? QStringLiteral("present") : QStringLiteral("MISSING"),
        files.dllPresent ? DiagnosticItem::Status::Ok : DiagnosticItem::Status::Problem,
        files.dllPresent ? files.dllPath
                         : QStringLiteral("Expected next to Flowbit.exe at:\n%1")
                               .arg(files.dllPath));

    add(QStringLiteral("Driver files"), QStringLiteral("WinDivert64.sys"),
        files.sysPresent ? QStringLiteral("present") : QStringLiteral("MISSING"),
        files.sysPresent ? DiagnosticItem::Status::Ok : DiagnosticItem::Status::Problem,
        files.sysPresent
            ? QStringLiteral("%1\nsignature: %2").arg(files.sysPath, files.signatureStatus)
            : QStringLiteral("Expected next to Flowbit.exe at:\n%1").arg(files.sysPath));

    if (files.sysPresent) {
        // The signer is reported without judgement: it is a fact about the build
        // that the user should be able to see for themselves.
        add(QStringLiteral("Driver files"), QStringLiteral("Driver signature"),
            files.signatureStatus,
            files.signatureStatus == QLatin1String("valid")
                ? DiagnosticItem::Status::Ok
                : DiagnosticItem::Status::Problem,
            QStringLiteral(
                "A packet-shaping driver is a kernel component, so Windows verifies "
                "its signature. Some security products also flag this class of driver "
                "as potentially unwanted, because the same technique is used to "
                "intercept traffic maliciously."));
    }

    // ---- driver service ---------------------------------------------------
    const ServiceState svc = queryWinDivertService();

    if (!svc.present) {
        add(QStringLiteral("Driver service"), QStringLiteral("WinDivert service"),
            QStringLiteral("not registered"), DiagnosticItem::Status::Problem,
            QStringLiteral(
                "The driver service is not present. Flowbit registers it during installation, "
                "and again on launch if it is found missing, so this means both attempts were "
                "refused. Registering a driver requires administrator rights: run Flowbit as "
                "Administrator once, from the folder it is installed in, and check this report "
                "again. If it still fails, a security product may be blocking the driver."));
    } else {
        const bool disabled = svc.startType == 4;
        add(QStringLiteral("Driver service"), QStringLiteral("Registered"),
            QStringLiteral("yes"), DiagnosticItem::Status::Info);

        // Whether the driver is usable RIGHT NOW is a separate question from its start
        // type. A resident instance from an earlier session keeps working even when the
        // service is flagged disabled, so the start type alone must not be reported as
        // a fault - measured: the driver opened with the engine's own filters while the
        // service read DISABLED.
        const bool loaded = svc.running;
        const bool blocked = disabled && !loaded;

        add(QStringLiteral("Driver service"), QStringLiteral("Start type"),
            describeStartType(svc.startType),
            blocked ? DiagnosticItem::Status::Problem
                    : (disabled ? DiagnosticItem::Status::Info
                                : DiagnosticItem::Status::Ok),
            disabled ? (loaded
                            ? QStringLiteral(
                                  "WinDivert registers its own driver service with a disabled start type "
                                    "and loads the driver through its own installer, not through "
                                    "Windows' service starter - which is why the driver runs "
                                    "while the service reads DISABLED.\n\n"
                                    "Nothing is wrong here: the driver is loaded and limits "
                                    "apply. The value is reported because it is surprising, "
                                    "not because it needs changing.")
                            : QStringLiteral(
                                  "The service is set not to start and the driver is not "
                                  "loaded, so limits cannot be applied. This is what an "
                                  "update leaves behind: Windows disables a kernel "
                                  "service whose driver file is replaced underneath "
                                  "it.\n\n"
                                  "Flowbit repairs this automatically when a limit is "
                                  "applied. If it does not, run this in an "
                                  "Administrator Command Prompt and restart Flowbit:\n"
                                  "    sc config WinDivert start= demand"))
                     : QString());

        add(QStringLiteral("Driver service"), QStringLiteral("Currently loaded"),
            svc.running ? QStringLiteral("yes") : QStringLiteral("no"),
            DiagnosticItem::Status::Info,
            (svc.running && disabled)
                ? QStringLiteral(
                      "The driver is still resident in the kernel from an earlier "
                      "session, which is why it can read as loaded while new requests "
                      "to open it still fail. A restart clears it.")
                : QString());

        if (!svc.imagePath.isEmpty()) {
            const QString registered = QDir::fromNativeSeparators(
                svc.imagePath.startsWith(QLatin1String("\\??\\"))
                    ? svc.imagePath.mid(4)
                    : svc.imagePath);
            const bool exists = QFileInfo::exists(registered);
            add(QStringLiteral("Driver service"), QStringLiteral("Registered file"),
                registered,
                exists ? DiagnosticItem::Status::Info : DiagnosticItem::Status::Problem,
                exists ? QString()
                       : QStringLiteral(
                             "The service points at a driver file that no longer exists "
                             "at that path. This is the usual reason a driver service "
                             "ends up disabled: the file was moved or replaced after "
                             "registration."));
        }
    }

    // ---- monitoring -------------------------------------------------------
    // The per-application activity panel uses an ETW trace session, a different
    // subsystem from the driver above. Its failures look identical to "nothing is
    // happening" unless they are reported, so they are reported here too.
    {
        const bool leftover = AppMonitor::leftoverSessionExists() && !monitorRunning;
        // Measured, not inferred: the report now names which sources were tested and
    // what each answered. Every misdiagnosis of this feature came from reading a
    // single error code and imagining a cause; this row replaces that with a test
    // result.
    add(QStringLiteral("Monitoring"), QStringLiteral("Data sources tested"),
        AppMonitor::dataSourceReport(),
        DiagnosticItem::Status::Info,
        QStringLiteral(
            "Each source was exercised directly. \"ETW session: works\" means a real "
            "trace session was created and stopped during this check. If ETW is "
            "blocked, the TCP EStats source is used instead: the per-application "
            "figures are still real, but UDP and QUIC traffic is not counted."));

    add(QStringLiteral("Monitoring"), QStringLiteral("Leftover trace session"),
            leftover ? QStringLiteral("yes - from an earlier run")
                     : (monitorRunning ? QStringLiteral("active (this session)")
                                       : QStringLiteral("none")),
            leftover ? DiagnosticItem::Status::Warning : DiagnosticItem::Status::Ok,
            leftover
                ? QStringLiteral(
                      "An earlier Flowbit run was closed without releasing its "
                      "session. Flowbit normally clears it when monitoring starts; "
                      "if the activity panel reports \"already in use\", clear it "
                      "with this in an Administrator Command Prompt:\n"
                      "    logman stop %1")
                      .arg(AppMonitor::sessionName())
                : QString());

        // ETW delivery counters: the pair separates 'provider silent' from
        // 'payload schema changed' - entirely different fixes.
        // Zero events means four different things, and only one of them is a fault.
        //
        // This row used to report a zero as a WARNING unconditionally: "the kernel network
        // provider is delivering no events on this Windows build". That was shown on every launch
        // where the activity panel had not been opened - because the monitor is what starts the
        // trace session, and with no session there are no events by definition. A user reading it
        // saw a fault on a perfectly healthy machine, which is the same mistake the activity panel
        // made before it learned to distinguish silence from breakage.
        //
        //   monitor never started   -> nothing is wrong; say how to start it
        //   running, no traffic yet -> normal on an idle machine for the first seconds
        //   running a while, still 0 -> genuinely suspicious: warn, with the duration
        //   events but none decoded -> the decoder does not match this build: a problem
        const bool noEvents = eventsSeen == 0;
        const QString monitorState =
            !monitorRunning
                ? QStringLiteral("The monitor is not running, so nothing is being traced. Open "
                                 "Application Network Activity to start it; this row then counts "
                                 "what arrives.")
                : (noEvents && monitorSeconds < 15
                       ? QStringLiteral("Monitoring has been running for %1s and nothing has "
                                        "transferred in that time, so there is nothing to count "
                                        "yet.").arg(monitorSeconds)
                       : (noEvents
                              ? QStringLiteral("Monitoring has been running for %1s and the kernel "
                                               "network provider has delivered no events. Send "
                                               "the saved report - the decoder may need this "
                                               "build's schema.").arg(monitorSeconds)
                              : QString()));

        add(QStringLiteral("Monitoring"), QStringLiteral("ETW events received"),
            monitorRunning ? QStringLiteral("%1 (decoded: %2), %3s")
                                 .arg(eventsSeen).arg(eventsDecoded).arg(monitorSeconds)
                           : QStringLiteral("%1 (decoded: %2), monitor not running")
                                 .arg(eventsSeen).arg(eventsDecoded),
            !monitorRunning                                 ? DiagnosticItem::Status::Info
            : (noEvents && monitorSeconds < 15)             ? DiagnosticItem::Status::Info
            : noEvents                                      ? DiagnosticItem::Status::Warning
                                                            : DiagnosticItem::Status::Ok,
            monitorState);

        // Phase 0.1 evidence: raw address bits and the local address list.
        // Placed first in MONITORING because it is what the direction fix is
        // derived from.
        if (!rawBits.isEmpty()) {
            add(QStringLiteral("Monitoring"),
                QStringLiteral("RAW address bits (Phase 0.1)"),
                QStringLiteral("hex, both byte orders, ports"),
                DiagnosticItem::Status::Info, rawBits);
        }

        if (!rawEvents.isEmpty()) {
            add(QStringLiteral("Monitoring"), QStringLiteral("Real traffic samples"),
                QStringLiteral("non-loopback events, raw fields"),
                DiagnosticItem::Status::Info, rawEvents);
        }

        if (!addressMiss.isEmpty()) {
            add(QStringLiteral("Monitoring"), QStringLiteral("Direction samples"),
                QStringLiteral("addresses that did not decide direction"),
                DiagnosticItem::Status::Info, addressMiss);
        }

        // Per-event-id schema + counts. This is the record that identifies a
        // field rename on a new Windows build in one paste.
        if (!eventLayout.isEmpty()) {
            add(QStringLiteral("Monitoring"), QStringLiteral("ETW event layout"),
                QStringLiteral("per event id"),
                DiagnosticItem::Status::Info,
                eventLayout);
        }

        if (eventsSeen > 0 && eventsDecoded == 0) {
            add(QStringLiteral("Monitoring"), QStringLiteral("Payload decode"),
                QStringLiteral("events arrive but none decode"),
                DiagnosticItem::Status::Problem,
                QStringLiteral("This Windows build's payload schema differs from the "
                               "decoder. Send the saved report - the field names and "
                               "sizes in it identify exactly what changed."));
        }

        if (!monitorError.isEmpty()) {
            add(QStringLiteral("Monitoring"), QStringLiteral("Last start failure"),
                monitorError, DiagnosticItem::Status::Problem,
                QStringLiteral("The Application Network Activity panel shows this "
                               "same reason with a Retry button."));
        }
    }

    // ---- feature state ----------------------------------------------------
    // Reported so a user can tell "not configured" from "configured but not
    // working": from the outside those two look identical, and only one of them is
    // a reason to go looking for a fault.
    return r;
}

} // namespace net
