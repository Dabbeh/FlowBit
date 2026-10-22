#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>
#include <sddl.h>

namespace net {
namespace autostart {

namespace {

// HKEY_CURRENT_USER, so no elevation is needed to change it and it applies to this user only.
const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// The name both mechanisms register under. "Flowbit" is what a user would look for in Task
// Manager's startup list.
//
// Held in a variable rather than a constant only so tests can point the real code paths at a
// throwaway name; see autostart::setRegistrationName(). Nothing in the application changes it.
QString g_registrationName = QStringLiteral("Flowbit");

QString registrationName()
{
    return g_registrationName;
}

// Reads the stored command, or an empty string when there is none.
QString readValue()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return QString();

    wchar_t buffer[2048] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const std::wstring name = registrationName().toStdWString();
    const LONG result = RegQueryValueExW(key, name.c_str(), nullptr, &type,
                                         reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_SZ)
        return QString();
    return QString::fromWCharArray(buffer);
}

bool writeValue(const QString &command, QString *error)
{
    HKEY key = nullptr;
    const LONG opened =
        RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr);
    if (opened != ERROR_SUCCESS) {
        if (error) {
            *error = QStringLiteral("Could not open the startup registry key (error %1).")
                         .arg(opened);
        }
        return false;
    }

    const std::wstring wide = command.toStdWString();
    const std::wstring name = registrationName().toStdWString();
    const LONG written =
        RegSetValueExW(key, name.c_str(), 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(wide.c_str()),
                       DWORD((wide.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);

    if (written != ERROR_SUCCESS) {
        if (error) {
            *error = QStringLiteral("Could not write the startup entry (error %1).")
                         .arg(written);
        }
        return false;
    }
    return true;
}

bool deleteValue(QString *error)
{
    HKEY key = nullptr;
    const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (opened != ERROR_SUCCESS) {
        // Nothing to remove is success: the desired state is "not registered".
        if (opened == ERROR_FILE_NOT_FOUND)
            return true;
        if (error)
            *error = QStringLiteral("Could not open the startup registry key (error %1).")
                         .arg(opened);
        return false;
    }

    const std::wstring name = registrationName().toStdWString();
    const LONG removed = RegDeleteValueW(key, name.c_str());
    RegCloseKey(key);

    // A missing value means it is already not registered.
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        if (error) {
            *error = QStringLiteral("Could not remove the startup entry (error %1).")
                         .arg(removed);
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The scheduled task.
//
// schtasks.exe rather than the Task Scheduler COM API: the API needs a large amount of
// boilerplate to express the same thing, and schtasks is present on every supported Windows.
// The cost is parsing, which is kept to a minimum - the exit code is the answer, and /Query is
// asked for the task by name.
//
// WHY /RL HIGHEST MATTERS
//
// Without it the task runs with the logged-on user's filtered token, which is exactly what an
// unelevated Flowbit has - and an unelevated Flowbit cannot load its driver. With it, Windows
// runs the task elevated at logon and, because Windows is the launcher and the task was
// registered by an already-elevated process, no consent prompt is shown. That is the whole
// point of using a task instead of the Run key.
// ---------------------------------------------------------------------------

// schtasks writes to stdout in whichever form suits the console it is attached to, and with
// /XML that can be a UTF-16 document. Decoded by inspection rather than assumption: a
// UTF-16LE stream of ASCII text has a NUL in every odd byte, which ANSI output never does.
QString decodeProcessOutput(const QByteArray &raw)
{
    const int limit = std::min<int>(raw.size() & ~1, 256);
    int pairs = 0;
    int zeroHighBytes = 0;
    for (int i = 1; i < limit; i += 2) {
        ++pairs;
        if (raw.at(i) == 0)
            ++zeroHighBytes;
    }
    if (pairs > 0 && zeroHighBytes == pairs) {
        return QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData()),
                                  raw.size() / 2);
    }
    return QString::fromLocal8Bit(raw);
}

// Runs schtasks and returns true on exit code 0.
bool runSchtasks(const QStringList &args, QString *output, QString *error)
{
    QProcess p;
    p.setProgram(QStringLiteral("schtasks.exe"));
    p.setArguments(args);
    // No shell, so nothing can be interpreted as a command by cmd.exe.
    p.start();
    if (!p.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("Could not run schtasks.exe.");
        return false;
    }
    p.waitForFinished(20000);

    const QString out = decodeProcessOutput(p.readAllStandardOutput())
                        + decodeProcessOutput(p.readAllStandardError());
    if (output)
        *output = out;

    if (p.exitStatus() != QProcess::NormalExit) {
        if (error)
            *error = QStringLiteral("schtasks did not finish.");
        return false;
    }
    if (p.exitCode() != 0) {
        if (error) {
            // schtasks reports the reason in its output; passing it through is more useful
            // than a bare exit code, which is meaningless to the user.
            const QString trimmed = out.trimmed();
            *error = trimmed.isEmpty()
                         ? QStringLiteral("schtasks failed with code %1.").arg(p.exitCode())
                         : trimmed;
        }
        return false;
    }
    return true;
}

// Minimal XML escaping for the values that end up inside the task definition. Paths rarely
// contain these, but a task definition that is silently malformed because one did would be
// indistinguishable from the failure this whole file exists to avoid.
QString xmlEscape(QString s)
{
    s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    s.replace(QLatin1Char('\''), QLatin1String("&apos;"));
    return s;
}

// The current user as a SID string (S-1-5-21-...).
//
// The SID rather than "DOMAIN\\user": it needs no escaping in XML, is unambiguous for a
// local account whose domain is the machine name, and is what Task Scheduler itself writes
// when it exports a task.
QString currentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return QString();

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QString sid;
    if (size > 0) {
        std::vector<BYTE> buffer(size);
        if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
            const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
            wchar_t *text = nullptr;
            if (ConvertSidToStringSidW(user->User.Sid, &text)) {
                sid = QString::fromWCharArray(text);
                LocalFree(text);
            }
        }
    }
    CloseHandle(token);
    return sid;
}

// The scheduled task, expressed as XML.
//
// WHY XML AND NOT /TR
//
// schtasks /Create /TR cannot carry a quoted program path together with arguments. Its
// parser re-splits the value on whitespace, so the command this application actually needs -
//   "C:\Program Files\Flowbit\Flowbit.exe" --tray-start
// arrived as a /TR value of "C:\Program" plus a stray token "Files\Flowbit\Flowbit.exe
// --tray-start", and the create was rejected with "Invalid argument/option - 'Files\...'".
// That is not a backslash-counting problem a different quoting form fixes; it is how the
// switch is parsed.
//
// The consequence was silent and total. "Program Files" contains a space, so on a default
// install the task was NEVER created. setEnabled() then fell back to the Run key, which
// cannot elevate - so Flowbit either did not start at logon, or started unelevated and could
// not load its driver.
//
// In XML the program and its arguments are separate elements, so no quoting is involved at
// all, and the trigger, principal and run level are explicit properties instead of switches
// that have to agree with one another.
QString taskXml(const QString &command, const QString &arguments, const QString &userId)
{
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
               "<Task version=\"1.2\" "
               "xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
               "  <RegistrationInfo>\n"
               "    <Description>Starts Flowbit minimised to the notification area at logon, "
               "so limits apply without opening a window.</Description>\n"
               "  </RegistrationInfo>\n"
               "  <Triggers>\n"
               "    <LogonTrigger>\n"
               "      <Enabled>true</Enabled>\n"
               "      <UserId>%1</UserId>\n"
               "    </LogonTrigger>\n"
               "  </Triggers>\n"
               "  <Principals>\n"
               "    <Principal id=\"Author\">\n"
               "      <UserId>%1</UserId>\n"
               "      <LogonType>InteractiveToken</LogonType>\n"
               "      <RunLevel>HighestAvailable</RunLevel>\n"
               "    </Principal>\n"
               "  </Principals>\n"
               "  <Settings>\n"
               "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
               "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
               "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
               "    <AllowHardTerminate>false</AllowHardTerminate>\n"
               "    <StartWhenAvailable>false</StartWhenAvailable>\n"
               "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
               "    <IdleSettings>\n"
               "      <StopOnIdleEnd>false</StopOnIdleEnd>\n"
               "      <RestartOnIdle>false</RestartOnIdle>\n"
               "    </IdleSettings>\n"
               "    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
               "    <Enabled>true</Enabled>\n"
               "    <Hidden>false</Hidden>\n"
               "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
               "    <WakeToRun>false</WakeToRun>\n"
               "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
               "    <Priority>7</Priority>\n"
               "  </Settings>\n"
               "  <Actions Context=\"Author\">\n"
               "    <Exec>\n"
               "      <Command>%2</Command>\n"
               "      <Arguments>%3</Arguments>\n"
               "    </Exec>\n"
               "  </Actions>\n"
               "</Task>\n")
        .arg(xmlEscape(userId), xmlEscape(command), xmlEscape(arguments));
}

// True when the registered task's action actually runs `command`.
//
// Read back through /XML so the check is of what Task Scheduler stored, not of what was
// intended. This is what turns "the task exists" into "the task will start this program".
bool taskRunsExecutable(const QString &command)
{
    QString out;
    QString ignored;
    if (!runSchtasks({QStringLiteral("/Query"), QStringLiteral("/TN"), taskName(),
                      QStringLiteral("/XML")},
                     &out, &ignored))
        return false;

    const QString native = QDir::toNativeSeparators(command);
    return out.contains(xmlEscape(native), Qt::CaseInsensitive);
}

} // namespace

QString executablePath()
{
    // The running executable, which is the one worth starting again. QDir::toNativeSeparators
    // because the registry and the task both want a Windows path, not the forward-slash form.
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

QString registeredCommand()
{
    // --tray-start tells the application it was launched by Windows rather than by the user,
    // so it goes straight to the notification area instead of opening a window on every logon.
    return QStringLiteral("\"%1\" --tray-start").arg(executablePath());
}

QString taskName()
{
    // Under the user's own tasks rather than the machine's, so creating it needs no
    // administrative rights and it applies to this user only.
    return registrationName();
}

void setRegistrationName(const QString &name)
{
    const QString wanted = name.trimmed();
    g_registrationName = wanted.isEmpty() ? QStringLiteral("Flowbit") : wanted;
}

bool taskExists()
{
    QString out;
    QString error;
    // /Query returns 0 when the task is present, non-zero when it is not.
    return runSchtasks({QStringLiteral("/Query"), QStringLiteral("/TN"), taskName()}, &out,
                       &error);
}

bool createTask(QString *error)
{
    // Defined by XML rather than by /TR, /SC, /RL and /IT - see taskXml() for why the
    // switch form could not express this command at all.
    const QString command = QDir::toNativeSeparators(executablePath());
    if (command.isEmpty()) {
        if (error)
            *error = QStringLiteral("Flowbit could not determine its own executable path.");
        return false;
    }

    const QString sid = currentUserSid();
    if (sid.isEmpty()) {
        if (error)
            *error = QStringLiteral("Flowbit could not read the current user's identifier, "
                                    "which the startup task needs.");
        return false;
    }

    const QString xml =
        taskXml(command, QStringLiteral("--tray-start"), sid);

    // Task Scheduler reads /XML definitions as Unicode, and that is what it writes itself.
    // Written as UTF-16LE with a byte-order mark so the declaration in the document is true
    // on disk rather than merely claimed.
    const QString path = QDir::toNativeSeparators(
        QDir(QDir::tempPath()).filePath(QStringLiteral("flowbit-startup-task.xml")));
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) {
                *error = QStringLiteral("Could not write the startup task definition to %1.")
                             .arg(path);
            }
            return false;
        }

        QByteArray bytes;
        bytes.append(char(0xFF));
        bytes.append(char(0xFE));
        const ushort *units = xml.utf16();
        bytes.append(reinterpret_cast<const char *>(units), int(xml.size() * 2));

        if (file.write(bytes) != bytes.size()) {
            file.close();
            QFile::remove(path);
            if (error)
                *error = QStringLiteral("Could not finish writing the startup task definition.");
            return false;
        }
        file.close();
    }

    // /F still applies with /XML: overwrite an existing task of the same name, so re-enabling
    // is not an error and the path is corrected after Flowbit is moved.
    const bool created = runSchtasks({QStringLiteral("/Create"), QStringLiteral("/TN"),
                                      taskName(), QStringLiteral("/XML"), path,
                                      QStringLiteral("/F")},
                                     nullptr, error);
    QFile::remove(path);

    // Read the task back before claiming success.
    //
    // A create that reports success but registers a different command is the failure mode
    // this file was rewritten to eliminate, so it is checked rather than assumed. Without the
    // check the settings page said "starts at logon with no prompt" for a task that started
    // the wrong thing, or nothing at all.
    if (created && !taskRunsExecutable(command)) {
        if (error) {
            *error = QStringLiteral(
                "The startup task was created but does not point at this copy of Flowbit.");
        }
        return false;
    }

    return created;
}

bool removeTask(QString *error)
{
    QString out;
    QString reason;
    if (runSchtasks({QStringLiteral("/Delete"), QStringLiteral("/TN"), taskName(),
                     QStringLiteral("/F")},
                    &out, &reason))
        return true;

    // Deleting a task that is not there is not a failure: the desired state is "no task".
    if (reason.contains(QLatin1String("cannot find"), Qt::CaseInsensitive)
        || reason.contains(QLatin1String("does not exist"), Qt::CaseInsensitive))
        return true;

    if (error)
        *error = reason;
    return false;
}

QString activeMechanism()
{
    if (taskExists())
        return QStringLiteral("task");
    const QString stored = readValue();
    if (!stored.isEmpty() && stored.compare(registeredCommand(), Qt::CaseInsensitive) == 0)
        return QStringLiteral("run");
    return QString();
}

QString registeredValue()
{
    return readValue();
}

bool isEnabled()
{
    // Either mechanism counts.
    //
    // Reporting "off" while a Run entry existed would be worse than a wrong label: the user
    // would turn the setting off, watch Flowbit start at the next logon anyway, and conclude
    // the option does not work.
    if (taskExists())
        return true;

    const QString stored = readValue();
    if (stored.isEmpty())
        return false;

    // Compared in full rather than merely checking that the value exists: if Flowbit has been
    // moved, the old command points at the old location, and reporting "enabled" would leave
    // the user believing it starts when it does not.
    return stored.compare(registeredCommand(), Qt::CaseInsensitive) == 0;
}

bool setEnabled(bool enabled, QString *error)
{
    if (!enabled) {
        // Both are removed, not just the one believed to be active. A leftover from a
        // previous version would otherwise keep starting Flowbit after it was switched off.
        QString taskError;
        const bool taskGone = removeTask(&taskError);
        const bool runGone = deleteValue(error);
        if (!taskGone && error && error->isEmpty())
            *error = taskError;
        return taskGone && runGone;
    }

    // The scheduled task is tried first because it is the one that avoids the logon prompt.
    QString taskError;
    if (createTask(&taskError)) {
        // The Run key is removed once the task exists, so there is one mechanism rather than
        // two trying to start the same application. Two would work - the single-instance guard
        // stops the second - but leaving both makes it impossible to tell which is in use.
        QString ignored;
        deleteValue(&ignored);
        return true;
    }

    // A task could not be created - most often because a managed machine forbids it. The Run
    // key still works, with the caveat that logon will show a consent prompt, so it is used
    // rather than leaving the setting doing nothing. The caller is told which happened so the
    // settings page can be honest about it.
    if (error)
        *error = QStringLiteral("__fallback__%1").arg(taskError);

    QString runError;
    if (!writeValue(registeredCommand(), &runError)) {
        if (error)
            *error = runError;
        return false;
    }
    return true;
}

} // namespace autostart
} // namespace net
