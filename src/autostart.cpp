#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>

#include <windows.h>

namespace net {
namespace autostart {

namespace {

// HKEY_CURRENT_USER, so no elevation is needed to change it and it applies to this user only.
const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// The value name. "Flowbit" is what a user would look for in Task Manager's startup list.
const wchar_t *kValueName = L"Flowbit";

// Reads the stored command, or an empty string when there is none.
QString readValue()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return QString();

    wchar_t buffer[2048] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, kValueName, nullptr, &type,
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
    const LONG written =
        RegSetValueExW(key, kValueName, 0, REG_SZ,
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

    const LONG removed = RegDeleteValueW(key, kValueName);
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

    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput())
                        + QString::fromLocal8Bit(p.readAllStandardError());
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
    return QStringLiteral("Flowbit");
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
    // /SC ONLOGON   - runs when the user logs on.
    // /RL HIGHEST   - elevated, so the driver can load and no prompt is shown. This is what
    //                 makes the difference from the Run key.
    // /F            - overwrite an existing task of the same name, so re-enabling is not an
    //                 error and the path is corrected after Flowbit is moved.
    // /IT           - runs in the interactive session, which a tray application needs.
    //
    // The command is passed as one quoted argument because it contains spaces and a switch.
    return runSchtasks({QStringLiteral("/Create"),
                        QStringLiteral("/TN"), taskName(),
                        QStringLiteral("/TR"), registeredCommand(),
                        QStringLiteral("/SC"), QStringLiteral("ONLOGON"),
                        QStringLiteral("/RL"), QStringLiteral("HIGHEST"),
                        QStringLiteral("/IT"),
                        QStringLiteral("/F")},
                       nullptr, error);
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
