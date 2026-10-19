#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
class QWidget;

namespace net {

// Checking for, downloading and installing a newer Flowbit, from inside the application.
//
// WHY THE UPDATE IS APPLIED BY A SEPARATE PROCESS
//
// Flowbit cannot replace its own executable: on Windows a running image is locked, so the
// file cannot be overwritten while it is in use. The established solution - and the one
// already proven in this project by apply-update.ps1 - is to hand the job to a small
// process that waits for Flowbit to exit, swaps the file, and starts it again.
//
// A script rather than a second copy of Flowbit is deliberate: the helper has to be able
// to overwrite Flowbit.exe, and a helper that IS Flowbit.exe could not overwrite itself
// either. PowerShell is present on every supported Windows and needs no dependency
// shipped with the application.
//
// WHERE THE VERSION COMES FROM
//
// A small JSON file in the repository, read over HTTPS:
//
//     {
//       "version": "1.1.0",
//       "url": "https://github.com/.../releases/download/v1.1.0/Flowbit.exe",
//       "notes": "Optional, shown before installing."
//     }
//
// Publishing an update is therefore: upload the executable to a release, edit one line of
// that file. A full releases API would work too, but it needs the release to be tagged in
// a particular way and gives nothing extra here.
//
// NOTHING IS INSTALLED WITHOUT CONSENT. The check only reads the small JSON file; the
// download begins only after the user accepts the offered version, and the swap only
// after the download completes and its size is plausible.
class Updater : public QObject
{
    Q_OBJECT

public:
    explicit Updater(QWidget *parentForDialogs);

    // Where the version file lives. Overridable so a build can be pointed at a test file
    // without touching the release URL.
    static QString versionFileUrl();
    static void setVersionFileUrl(const QString &url);

    // Reads the version file and compares it with the running version. Emits
    // updateAvailable() or upToDate(); never downloads anything.
    void checkForUpdate();

    // Offers an update to the user and, if accepted, downloads and installs it.
    //
    // Public because the settings page calls it: the version check and the decision to
    // install are separate steps, and the check only reports what it found. Nothing is
    // transferred until this runs.
    void offerUpdate(const QString &version, const QString &url, const QString &notes);

    // The running version, as reported by the application.
    static QString currentVersion();

    // True when `candidate` is a later version than the one running. Compares the numeric
    // parts rather than the strings, so "1.10.0" is correctly newer than "1.9.0" - a
    // string comparison would get that wrong.
    static bool isNewer(const QString &candidate);

signals:
    void updateAvailable(const QString &version, const QString &url, const QString &notes);

    // The user asked to be reminded later, or to stop being offered this version.
    //
    // Separate signals rather than a return value because the offer is shown from a dialog the
    // caller does not manage: the choice has to travel to whoever owns the settings.
    void updateDeferred(const QString &version);
    void updateSkipped(const QString &version);
    void upToDate(const QString &version);
    void failed(const QString &reason);

private:
    void onVersionReply(QNetworkReply *reply);
    void beginDownload(const QString &version, const QString &url, const QString &notes);
    void installDownloaded(const QString &version, const QString &path);

    QWidget *m_ui = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QProgressDialog *m_progress = nullptr;
    QString m_pendingVersion;
};

} // namespace net
