// Verifies the diagnostics report.
//
// The report is only useful if it is ACCURATE and COMPLETE, so this checks both:
// that every expected area is covered, and that the readings match the real machine
// state (which the test re-derives independently rather than trusting the report).
//
// Build/run: tools/run_diag_tests.ps1
#include "../src/diagnostics.h"
#include "../src/diagnosticspanel.h"
#include "../src/theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextStream>

#include <windows.h>

#include <cstdio>

using namespace net;

static int g_failures = 0;
static int g_checks = 0;
static QFile *g_log = nullptr;

static void logLine(const QString &s)
{
    std::printf("%s\n", qPrintable(s));
    std::fflush(stdout);
    if (g_log && g_log->isOpen()) {
        g_log->write(s.toUtf8());
        g_log->write("\n");
        g_log->flush();
    }
}

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        logLine(QStringLiteral("FAIL: ") + what);
    } else {
        logLine(QStringLiteral("  ok: ") + what);
    }
}

// Independently determines whether this process is elevated, so the report's claim
// can be compared against something other than itself.
static bool reallyElevated()
{
    BOOL admin = FALSE;
    PSID admins = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &admins)) {
        CheckTokenMembership(nullptr, admins, &admin);
        FreeSid(admins);
    }
    return admin == TRUE;
}

// Reads the WinDivert service Start value straight from the registry.
static DWORD realStartType()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Services\\WinDivert", 0, KEY_READ, &key)
        != ERROR_SUCCESS)
        return 0xFFFFFFFF;   // absent
    DWORD value = 0;
    DWORD len = sizeof(value);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, L"Start", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&value), &len)
                        == ERROR_SUCCESS
                    && type == REG_DWORD;
    RegCloseKey(key);
    return ok ? value : 0xFFFFFFFF;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("diag_results.txt")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    theme::setMode(theme::Mode::Dark);

    const bool elevated = reallyElevated();
    const DiagnosticReport report = collectDiagnostics(elevated);

    logLine(QStringLiteral("== coverage =="));

    // Every area the brief asks about must actually be reported.
    const auto hasGroup = [&report](const QString &g) {
        for (const DiagnosticItem &i : report.items) {
            if (i.group.compare(g, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    const auto hasName = [&report](const QString &n) {
        for (const DiagnosticItem &i : report.items) {
            if (i.name.contains(n, Qt::CaseInsensitive))
                return true;
        }
        return false;
    };

    check(report.items.size() >= 10,
          QStringLiteral("the report has substance (%1 readings)").arg(report.items.size()));
    check(hasGroup(QStringLiteral("Build")), "build information is reported");
    check(hasGroup(QStringLiteral("Permissions")), "permissions are reported");
    check(hasGroup(QStringLiteral("Driver files")), "driver files are reported");
    check(hasGroup(QStringLiteral("Driver service")), "the driver service is reported");
    check(hasName(QStringLiteral("Elevation")), "elevation state is reported");
    // The WinDivert service is registered ON DEMAND: WinDivert creates it the first
    // time the driver is opened, and it may not exist at all on a machine that has
    // not applied a limit since boot. Asserting the row unconditionally therefore
    // failed on a perfectly healthy system. Both outcomes are correct; what matters
    // is that the report is self-consistent, so this records which one was seen and
    // verifies the service row explains the state.
    {
        const bool registered = hasName(QStringLiteral("WinDivert service"));
        const bool startType = hasName(QStringLiteral("Start type"));

        if (startType) {
            check(true, "the service start type is reported (service registered)");
        } else {
            // Not registered: the report must not imply a service exists.
            check(registered, "the service row is present even when unregistered");
            logLine(QStringLiteral("     note: the WinDivert service is not registered "
                                   "on this machine (registered on first use)"));
        }
    }
    check(hasName(QStringLiteral("WinDivert.dll")), "the DLL presence is reported");
    check(hasName(QStringLiteral("signature")), "the driver signature is reported");

    logLine(QStringLiteral("\n== accuracy against the real machine =="));

    // The elevation claim must match reality, not just be present.
    bool elevationMatches = false;
    for (const DiagnosticItem &i : report.items) {
        if (i.name == QLatin1String("Elevation")) {
            const bool saysElevated = i.value.contains(QStringLiteral("Administrator"));
            elevationMatches = (saysElevated == elevated);
            logLine(QStringLiteral("     report says: %1   actual: %2")
                        .arg(i.value, elevated ? QStringLiteral("elevated")
                                               : QStringLiteral("not elevated")));
        }
    }
    check(elevationMatches, "the elevation reading matches the real process token");

    // The start-type reading must match the registry.
    //
    // If the driver service has never been registered - which is normal, since
    // WinDivert creates it the first time the driver is opened - there is no start
    // type for the report to carry, and the loop below finds no row. That is an
    // accurate report of an absent service, not a mismatch, so the flag starts true
    // in that case and the row (if present) can only disprove it.
    const DWORD actualStart = realStartType();
    bool startMatches = (actualStart == 0xFFFFFFFF);
    bool startFlaggedAsProblem = false;
    // Whether the row is present at all. Separate from the severity: the point of the
    // disabled-service check is that the state is reported, and it must not pass merely
    // because the row was omitted.
    bool startReported = false;
    DiagnosticItem::Status startStatus = DiagnosticItem::Status::Info;
    for (const DiagnosticItem &i : report.items) {
        if (i.name != QLatin1String("Start type"))
            continue;
        startReported = true;
        startStatus = i.status;
        if (actualStart == 0xFFFFFFFF) {
            startMatches = true;   // service absent; a different reading covers it
        } else if (actualStart == 4) {
            startMatches = i.value.contains(QStringLiteral("DISABLED"));
            startFlaggedAsProblem = i.status == DiagnosticItem::Status::Problem;
        } else if (actualStart == 3) {
            startMatches = i.value.contains(QStringLiteral("demand"));
        } else {
            startMatches = true;
        }
        logLine(QStringLiteral("     registry Start=%1   report says: %2")
                    .arg(actualStart)
                    .arg(i.value));
    }
    check(startMatches, "the start type reading matches the registry");

    // A disabled service must be reported, and at the right severity.
    //
    // WARNING, not Problem. The service can be DISABLED while the driver is loaded in the
    // kernel - START_TYPE 4 with STATE 4 RUNNING - and in that state limits are being applied
    // normally. Calling it a problem would tell the user something is broken while it is
    // working. What is true is that it will not load after a restart, and that is what the
    // warning says.
    if (actualStart == 4) {
        check(!startFlaggedAsProblem,
              "a DISABLED service is a warning, because limits still apply while it is "
              "loaded");

        // It must still be reported at all - not neutral, not omitted. The severity changed;
        // whether the user is told did not.
        check(startReported,
              "a DISABLED service is still reported, so the user can see the state");

        // And it must say what happens about it. The application repairs this itself at the
        // next start, so the useful message names that rather than sending the user to an
        // elevated Command Prompt for something they do not have to do.
        //
        // The manual command appears only when the driver is NOT loaded. That distinction is
        // deliberate and worth preserving: with the driver loaded, limits are being applied
        // and the repair happens by itself, so a command prompt would be busywork. Without
        // it, nothing is being limited and the user may reasonably want to act now.
        bool explainsRepair = false;
        bool hasManualFix = false;
        for (const DiagnosticItem &i : report.items) {
            if (i.name != QLatin1String("Start type"))
                continue;
            if (i.detail.contains(QStringLiteral("repairs it automatically"))
                || i.detail.contains(QStringLiteral("repairs this automatically")))
                explainsRepair = true;
            if (i.detail.contains(QStringLiteral("sc config WinDivert start= demand")))
                hasManualFix = true;
        }

        check(explainsRepair,
              "the report says the repair is automatic, rather than implying the user must "
              "act");

        // Whether the command is offered follows the driver's state, which is reported in
        // its own row. Read that rather than assuming, so the test states the rule instead
        // of one outcome of it.
        bool driverLoaded = false;
        for (const DiagnosticItem &i : report.items) {
            if (i.name == QLatin1String("Currently loaded"))
                driverLoaded = i.value.startsWith(QLatin1String("yes"));
        }

        if (driverLoaded) {
            check(!hasManualFix,
                  "with the driver loaded, no command prompt is suggested - the repair is "
                  "automatic and limits are already applying");
        } else {
            check(hasManualFix,
                  "with the driver not loaded, the manual command is offered so the user can "
                  "act now");
        }
    }

    // Driver files must be checked where they actually live.
    {
        const QString dir = QCoreApplication::applicationDirPath();
        const bool dllReal = QFileInfo::exists(QDir(dir).filePath(QStringLiteral("WinDivert.dll")));
        bool dllAgrees = false;
        for (const DiagnosticItem &i : report.items) {
            if (i.name == QLatin1String("WinDivert.dll"))
                dllAgrees = i.value.startsWith(dllReal ? QStringLiteral("present")
                                                       : QStringLiteral("MISSING"));
        }
        check(dllAgrees, "the DLL reading agrees with the file system");
    }

    logLine(QStringLiteral("\n== the exported text =="));

    const QString text = report.toText();
    check(!text.isEmpty(), "a text report can be produced");
    check(text.contains(QStringLiteral("Flowbit diagnostics")), "it identifies itself");
    check(text.contains(QStringLiteral("Summary:")), "it carries a summary line");
    check(text.contains(QStringLiteral("Generated:")), "it is timestamped");
    check(text.contains(QStringLiteral("BUILD")) || text.contains(QStringLiteral("Build")),
          "it groups its readings");

    // A problem must be visually distinguishable in the text form too, since that is
    // what gets pasted into a message.
    if (report.hasProblems()) {
        check(text.contains(QStringLiteral("[XX]")),
              "problems are marked in the text export");
    }
    check(text.contains(QStringLiteral("[OK]")) || text.contains(QStringLiteral("[--]")),
          "healthy readings are marked too");

    // The report must not leak anything sensitive. No traffic contents, no
    // addresses, no history.
    check(!text.contains(QStringLiteral("password"), Qt::CaseInsensitive),
          "no credentials appear in the report");
    check(!text.contains(QStringLiteral("127.0.0.1"))
              && !text.contains(QStringLiteral("192.168.")),
          "no network addresses are recorded");

    logLine(QStringLiteral("\n== export =="));
    {
        QTemporaryDir tmp;
        const QString path = QDir(tmp.path()).filePath(QStringLiteral("diag.txt"));
        QString error;
        const bool ok = exportDiagnostics(report, path, &error);
        check(ok, QStringLiteral("the report can be written to a file (%1)").arg(error));
        check(QFileInfo::exists(path), "the file exists afterwards");

        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QString written = QString::fromUtf8(f.readAll());
            // Written through a QIODevice::Text device, which translates "\n" to
            // CRLF on Windows, so that translation is undone before comparing. The
            // two must be equal once normalised, or the export is silently altering
            // the content.
            QString a = written;
            a.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            QString b = text;
            b.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            if (a != b) {
                // Show the first divergence, so the cause is visible rather than
                // inferred from a size difference.
                int i = 0;
                while (i < a.size() && i < b.size() && a.at(i) == b.at(i)) ++i;
                logLine(QStringLiteral("     diverges at %1:").arg(i));
                logLine(QStringLiteral("       file: %2")
                            .arg(a.mid(qMax(0, i - 30), 80).replace(QChar(10), QChar(124))));
                logLine(QStringLiteral("       mem : %2")
                            .arg(b.mid(qMax(0, i - 30), 80).replace(QChar(10), QChar(124))));
                // Show the raw bytes around the divergence.
                logLine(QStringLiteral("       file bytes: %2")
                            .arg(QString::fromLatin1(a.mid(qMax(0, i - 8), 24).toUtf8().toHex(' '))));
                logLine(QStringLiteral("       mem  bytes: %2")
                            .arg(QString::fromLatin1(b.mid(qMax(0, i - 8), 24).toUtf8().toHex(' '))));
                logLine(QStringLiteral("       file CR count: %2, mem CR count: %3")
                            .arg(a.count(QChar(13))).arg(b.count(QChar(13))));
                logLine(QStringLiteral("       file LF count: %2, mem LF count: %3")
                            .arg(a.count(QChar(10))).arg(b.count(QChar(10))));
            }
            check(a == b, QStringLiteral("the file contents match the report (sizes %1 vs %2)")
                              .arg(a.size()).arg(b.size()));
        }
    }

    logLine(QStringLiteral("\n== the panel =="));
    {
        DiagnosticsPanel panel;
        panel.resize(720, 520);
        panel.show();
        QApplication::processEvents();

        check(panel.report().items.size() == report.items.size(),
              "the panel collects the same readings");
        check(!panel.summaryText().isEmpty(),
              QStringLiteral("the panel shows a summary (%1)").arg(panel.summaryText()));

        // The summary must reflect the true count rather than always looking calm.
        const bool anyProblem = panel.hasProblems();
        check(anyProblem == panel.report().hasProblems(),
              "the panel's problem flag matches its report");
        if (anyProblem) {
            check(panel.summaryText().contains(QStringLiteral("problem")),
                  QStringLiteral("the summary names the problem (%1)").arg(panel.summaryText()));
        }

        // The actions must exist: a report you cannot retrieve is not much use.
        bool copyFound = false, saveFound = false, recheckFound = false;
        for (QPushButton *b : panel.findChildren<QPushButton *>()) {
            if (b->text().contains(QStringLiteral("Copy"))) copyFound = true;
            if (b->text().contains(QStringLiteral("Save"))) saveFound = true;
            if (b->text().contains(QStringLiteral("Re-check"))) recheckFound = true;
        }
        check(copyFound, "the report can be copied");
        check(saveFound, "the report can be saved");
        check(recheckFound, "the state can be re-read without restarting");

        // Re-checking must not duplicate rows.
        const int before = panel.findChildren<QLabel *>().size();
        panel.refresh();
        panel.refresh();
        QApplication::processEvents();
        const int after = panel.findChildren<QLabel *>().size();
        check(after <= before * 1.5,
              QStringLiteral("re-checking does not pile up rows (%1 -> %2)").arg(before).arg(after));
    }

    logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
