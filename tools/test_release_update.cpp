// Exercises the production updater check and Qt HTTPS download without replacing
// an installed application. Pass feed URL, running version, and expected SHA-256.
#include "updater.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 4 && argc != 5) return 2;
    const QString expectedVersion = argc == 5 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("1.0.2");
    app.setApplicationVersion(QString::fromLocal8Bit(argv[2]));
    net::Updater::setVersionFileUrl(QUrl::fromUserInput(QString::fromLocal8Bit(argv[1])).toString());
    net::Updater updater(nullptr);
    QEventLoop loop;
    QString download;
    bool available = false, current = false, failed = false;
    QObject::connect(&updater, &net::Updater::updateAvailable,
        [&](const QString &version, const QString &url, const QString &) {
            available = version == expectedVersion; download = url; loop.quit();
        });
    QObject::connect(&updater, &net::Updater::upToDate, [&](const QString &version) {
        current = version == expectedVersion; loop.quit();
    });
    QObject::connect(&updater, &net::Updater::failed, [&](const QString &why) {
        std::printf("FAIL: %s\n", qPrintable(why)); failed = true; loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&] { failed = true; loop.quit(); });
    timeout.start(30000);
    updater.checkForUpdate();
    loop.exec();
    timeout.stop();
    const bool expectNew = app.applicationVersion() != expectedVersion;
    if (failed || (expectNew ? !available : !current)) return 1;
    std::printf("PASS: running %s receives the correct %s update result\n", argv[2], qPrintable(expectedVersion));
    if (!expectNew) return 0;
    QNetworkAccessManager network;
    QNetworkRequest request{QUrl(download)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000); // Match the app's inactivity timeout.
    auto *reply = network.get(request);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    auto consume = [&] { const auto chunk = reply->readAll(); bytes += chunk.size(); hash.addData(chunk); };
    QObject::connect(reply, &QNetworkReply::readyRead, consume);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // A slow but progressing download is valid. Bound the whole test separately
    // from the request's inactivity timeout rather than failing after one minute.
    timeout.start(10 * 60 * 1000);
    loop.exec();
    timeout.stop();
    consume();
    const bool ok = !failed && reply->error() == QNetworkReply::NoError && bytes > 200 * 1024
        && hash.result().toHex() == QByteArray(argv[3]).toLower();
    std::printf("%s: Qt downloads %lld bytes matching the release SHA-256\n",
                ok ? "PASS" : "FAIL", static_cast<long long>(bytes));
    if (!ok)
        std::printf("  network: %s; test deadline exceeded: %s\n",
                    qPrintable(reply->errorString()), failed ? "yes" : "no");
    delete reply;
    return ok ? 0 : 1;
}
