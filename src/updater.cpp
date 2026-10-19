#include "updater.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>

namespace net {

namespace {

// Default location of the version file, read from the repository over HTTPS.
//
// raw.githubusercontent.com serves the file as stored, which is what is wanted here: a
// small hand-edited document, not an API response that has to be parsed apart.
QString g_versionUrl =
    QStringLiteral("https://raw.githubusercontent.com/Dabbeh/FlowBit/main/docs/version.json");

// Shared with the existing deploy script so the two cannot disagree about the file names.
const char *kStagedExe = "Flowbit-update.exe";
const char *kTargetExe = "Flowbit.exe";

} // namespace

QString Updater::versionFileUrl()
{
    return g_versionUrl;
}

void Updater::setVersionFileUrl(const QString &url)
{
    g_versionUrl = url;
}

QString Updater::currentVersion()
{
    const QString v = QCoreApplication::applicationVersion();
    return v.isEmpty() ? QStringLiteral("0.0.0") : v;
}

bool Updater::isNewer(const QString &candidate)
{
    // Compared part by part as numbers.
    //
    // A plain string comparison would call "1.10.0" older than "1.9.0", because '1' sorts
    // before '9' - which would silently withhold an update, the worst kind of failure here
    // because nothing would appear wrong.
    const auto parts = [](const QString &v) {
        QVector<int> out;
        for (const QString &piece : v.split(QLatin1Char('.'))) {
            // Stop at anything non-numeric, so a suffix such as "-beta" is ignored rather
            // than turning the whole comparison into nonsense.
            bool ok = false;
            const int n = piece.trimmed().toInt(&ok);
            out.append(ok ? n : 0);
            if (!ok)
                break;
        }
        return out;
    };

    const QVector<int> have = parts(currentVersion());
    const QVector<int> want = parts(candidate);
    const int n = qMax(have.size(), want.size());
    for (int i = 0; i < n; ++i) {
        const int a = i < have.size() ? have.at(i) : 0;
        const int b = i < want.size() ? want.at(i) : 0;
        if (b != a)
            return b > a;
    }
    return false;   // equal
}

Updater::Updater(QWidget *parentForDialogs) : m_ui(parentForDialogs)
{
    m_net = new QNetworkAccessManager(this);
}

void Updater::checkForUpdate()
{
    QNetworkRequest request{QUrl(versionFileUrl())};
    // GitHub serves raw files with a short cache life; asking explicitly avoids a stale
    // "up to date" from a proxy.
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Flowbit/%1").arg(currentVersion()));

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onVersionReply(reply); });
}

void Updater::onVersionReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        // A failed check is not worth interrupting anyone over: the network may simply be
        // down, and nothing is broken. Reported through the signal so the settings page can
        // show it if the user asked explicitly, but no dialog appears on its own.
        emit failed(reply->errorString());
        return;
    }

    const QByteArray body = reply->readAll();
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit failed(QStringLiteral("The version file could not be read."));
        return;
    }

    const QJsonObject root = doc.object();
    const QString version = root.value(QStringLiteral("version")).toString();
    const QString url = root.value(QStringLiteral("url")).toString();
    const QString notes = root.value(QStringLiteral("notes")).toString();

    if (version.isEmpty() || url.isEmpty()) {
        emit failed(QStringLiteral("The version file is missing a version or a download "
                                   "address."));
        return;
    }

    if (!isNewer(version)) {
        emit upToDate(version);
        return;
    }

    emit updateAvailable(version, url, notes);
}

void Updater::offerUpdate(const QString &version, const QString &url, const QString &notes)
{
    beginDownload(version, url, notes);
}

void Updater::beginDownload(const QString &version, const QString &url, const QString &notes)
{
    m_pendingVersion = version;

    // The offer comes before any download, so nothing is transferred without consent -
    // including on a metered connection, where several megabytes is not a trivial cost.
    // Three answers, because two always lose one of them.
    //
    // "No" used to mean two different things at once - not now, and never again - so a user who
    // wanted to defer had to either accept the download immediately or be asked again on the next
    // check with no way to say "not this time but keep telling me". Each answer now has its own
    // button and its own consequence:
    //
    //   Update now         download and swap in
    //   Later              ask again on the next check, in this session or the next launch
    //   Skip this version  stop offering THIS version; a newer one is offered normally
    QMessageBox box(m_ui);
    box.setWindowTitle(QStringLiteral("Update available"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral("Flowbit %1 is available. You have %2.")
                    .arg(version, currentVersion()));
    if (!notes.isEmpty())
        box.setInformativeText(notes);
    box.setStandardButtons(QMessageBox::NoButton);

    QPushButton *nowButton = box.addButton(QStringLiteral("Update now"), QMessageBox::AcceptRole);
    QPushButton *laterButton = box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    QPushButton *skipButton =
        box.addButton(QStringLiteral("Skip this version"), QMessageBox::DestructiveRole);
    box.setDefaultButton(nowButton);
    box.setEscapeButton(laterButton);
    box.exec();

    if (box.clickedButton() == skipButton) {
        emit updateSkipped(version);
        return;
    }
    if (box.clickedButton() != nowButton) {
        // Later, or the window closed - both mean "not now". Closing the dialog is treated as
        // deferral rather than as a decision to skip: a stray Escape must not silence an update
        // for good.
        emit updateDeferred(version);
        return;
    }

    // Written beside the running executable, because that is where the swap has to happen
    // and it avoids needing to know where a temporary directory is writable.
    const QString staged =
        QDir(QCoreApplication::applicationDirPath()).filePath(QLatin1String(kStagedExe));

    QFile *file = new QFile(staged, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(m_ui, QStringLiteral("Update"),
                             QStringLiteral("Could not write next to Flowbit:\n%1")
                                 .arg(file->errorString()));
        file->deleteLater();
        return;
    }

    m_progress = new QProgressDialog(
        QStringLiteral("Downloading Flowbit %1...").arg(version),
        QStringLiteral("Cancel"), 0, 100, m_ui);
    m_progress->setWindowTitle(QStringLiteral("Updating Flowbit"));
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(0);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    m_progress->setValue(0);

    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Flowbit/%1").arg(currentVersion()));

    QNetworkReply *reply = m_net->get(request);

    connect(reply, &QNetworkReply::readyRead, this, [reply, file] {
        if (file->isOpen())
            file->write(reply->readAll());
    });

    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 got, qint64 total) {
                if (!m_progress || total <= 0)
                    return;
                m_progress->setMaximum(100);
                m_progress->setValue(int(got * 100 / total));
            });

    // Cancelling must stop the transfer and leave nothing behind, or a half-written
    // executable would be there for the next attempt to trip over.
    connect(m_progress, &QProgressDialog::canceled, this, [reply] {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, file, staged, version] {
        if (file->isOpen()) {
            file->write(reply->readAll());
            file->close();
        }
        reply->deleteLater();
        if (m_progress) {
            m_progress->close();
            m_progress->deleteLater();
            m_progress = nullptr;
        }

        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(staged);
            if (reply->error() != QNetworkReply::OperationCanceledError) {
                QMessageBox::warning(m_ui, QStringLiteral("Update"),
                                     QStringLiteral("The download did not finish:\n%1")
                                         .arg(reply->errorString()));
            }
            return;
        }

        // A file far smaller than an executable means an error page or a redirect that
        // returned HTML, not the program. Checking stops that being installed, which would
        // leave the user with a broken application.
        const qint64 size = QFileInfo(staged).size();
        if (size < 200 * 1024) {
            QFile::remove(staged);
            QMessageBox::warning(
                m_ui, QStringLiteral("Update"),
                QStringLiteral("The downloaded file is too small to be Flowbit, so it has "
                               "not been installed."));
            return;
        }

        installDownloaded(version, staged);
    });
}

void Updater::installDownloaded(const QString &version, const QString &path)
{
    Q_UNUSED(path)

    // The swap is done by a script rather than by Flowbit itself.
    //
    // Windows locks a running executable, so Flowbit cannot overwrite its own file. The
    // script waits for this process to exit, copies the staged file over the target,
    // verifies the copy by hash, and starts Flowbit again elevated. That last part
    // matters: a relaunch without elevation would leave the new instance unable to load
    // its driver, which is the failure this project has already met once.
    const QString dir = QCoreApplication::applicationDirPath();
    const QString scriptPath = QDir(dir).filePath(QStringLiteral("flowbit-self-update.ps1"));

    const QString script = QStringLiteral(R"PS(
$ErrorActionPreference = 'Continue'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir

# Wait for Flowbit to release its own image. Polled rather than timed, so a slow shutdown
# does not race the copy.
for ($i = 0; $i -lt 60; $i++) {
    if (-not (Get-Process Flowbit -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 500
}
Get-Process Flowbit -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

$staged = Join-Path $dir 'Flowbit-update.exe'
$target = Join-Path $dir 'Flowbit.exe'
if (-not (Test-Path $staged)) { exit 1 }

$want = (Get-FileHash $staged).Hash
$ok = $false
for ($c = 1; $c -le 5; $c++) {
    Copy-Item $staged $target -Force
    if ((Get-FileHash $target).Hash -eq $want) { $ok = $true; break }
    Start-Sleep -Milliseconds 700
}
if ($ok) { Remove-Item $staged -Force -ErrorAction SilentlyContinue }

# Relaunch elevated: an unelevated Flowbit cannot load its driver.
Start-Process -FilePath $target -Verb RunAs

# Remove this script last. It is running from the file being deleted, which Windows allows -
# the image is already loaded and the directory entry goes when the handle closes - and doing
# it here rather than leaving it means no helper accumulates in the install folder across
# a series of updates.
#
# Only removed after a successful copy: if the update failed, the script is the only record
# of what was attempted, and the install folder having it there is useful evidence rather
# than clutter.
if ($ok) { Remove-Item $MyInvocation.MyCommand.Path -Force -ErrorAction SilentlyContinue }
)PS");

    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(m_ui, QStringLiteral("Update"),
                             QStringLiteral("Could not prepare the installer:\n%1")
                                 .arg(scriptFile.errorString()));
        return;
    }
    scriptFile.write(script.toUtf8());
    scriptFile.close();

    QMessageBox::information(
        m_ui, QStringLiteral("Update ready"),
        QStringLiteral("Flowbit %1 has been downloaded and will be installed now.\n\n"
                       "Flowbit will close and start again in a moment.")
            .arg(version));

    // Detached, so the helper outlives this process - which it must, since its whole job
    // is to wait for Flowbit to exit.
    QProcess::startDetached(QStringLiteral("powershell.exe"),
                            {QStringLiteral("-NoProfile"),
                             QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                             QStringLiteral("-File"), scriptPath});

    QApplication::quit();
}

} // namespace net
