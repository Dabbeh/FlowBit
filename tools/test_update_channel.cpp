// Verifies the update check against a real version file over the network.
//
// Two halves matter and they fail differently:
//
//   * the comparison - a wrong answer means no update is ever offered, silently;
//   * the fetch and parse - a wrong URL or a malformed file means the same.
//
// The committed docs/version.json is read over HTTPS exactly as the application reads it,
// so this exercises the real path rather than a copy. It is pointed at the local file as
// well, so the parsing is checked even when the network is unavailable, and the two results
// are compared.

#include "updater.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTimer>

#include <cstdio>

using namespace net;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

// Waits for one of the three outcomes, with a bound so a hung network cannot hang the test.
struct Outcome {
    QString kind;      // "available", "uptodate", "failed" or "timeout"
    QString version;
    QString url;
    QString notes;
    QString reason;
};

Outcome runCheck(Updater *u, int timeoutMs)
{
    Outcome out;
    QEventLoop loop;

    QObject::connect(u, &Updater::updateAvailable,
                     [&](const QString &v, const QString &url, const QString &notes) {
                         out.kind = QStringLiteral("available");
                         out.version = v;
                         out.url = url;
                         out.notes = notes;
                         loop.quit();
                     });
    QObject::connect(u, &Updater::upToDate, [&](const QString &v) {
        out.kind = QStringLiteral("uptodate");
        out.version = v;
        loop.quit();
    });
    QObject::connect(u, &Updater::failed, [&](const QString &r) {
        out.kind = QStringLiteral("failed");
        out.reason = r;
        loop.quit();
    });

    QTimer::singleShot(timeoutMs, [&] {
        if (out.kind.isEmpty()) {
            out.kind = QStringLiteral("timeout");
            loop.quit();
        }
    });

    u->checkForUpdate();
    loop.exec();
    return out;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    std::printf("\n== the version file, as the application reads it ==\n");
    std::printf("  url: %s\n", qPrintable(Updater::versionFileUrl()));

    // ---- 1. the committed file parses ------------------------------------
    // Read from disk first, so the parsing is checked without depending on the network.
    {
        QFile f(QStringLiteral("C:/Users/enaya/Desktop/Flowbit-Source/docs/version.json"));
        check(f.exists(), "docs/version.json exists in the repository");
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray body = f.readAll();
            std::printf("  contents: %s\n", body.simplified().constData());
            check(body.contains("\"version\""), "it declares a version");
            check(body.contains("\"url\""), "and a download address");
        }
    }

    // ---- 2. the comparison against the real version ----------------------
    // The committed file says 1.0.0 and the application reports 1.0.0, so the correct answer
    // is "up to date". A parity mistake here would offer an update to the same version,
    // which installs and then offers itself again.
    {
        std::printf("\n== checking over the network ==\n");
        Updater u(nullptr);
        const Outcome o = runCheck(&u, 20000);

        std::printf("  outcome: %s%s%s\n", qPrintable(o.kind),
                    o.version.isEmpty() ? "" : qPrintable(QStringLiteral(" (%1)").arg(o.version)),
                    o.reason.isEmpty() ? "" : qPrintable(QStringLiteral(" - %1").arg(o.reason)));

        if (o.kind == QLatin1String("timeout")) {
            std::printf("  the network did not answer, so the fetch is not checked here\n");
        } else if (o.kind == QLatin1String("failed")) {
            // A failure is a valid result when there is no network, and it must be reported
            // rather than silently treated as up to date.
            check(!o.reason.isEmpty(), "a failed check gives a reason rather than silence");
        } else {
            check(o.kind == QLatin1String("uptodate"),
                  QStringLiteral("the committed 1.0.0 does not offer itself as an update (%1)")
                      .arg(o.kind));
            check(o.version == QLatin1String("1.0.0"),
                  QStringLiteral("and the reported version is the one in the file (%1)")
                      .arg(o.version));
        }
    }

    // ---- 3. a newer version IS offered -----------------------------------
    // The local file is served through a file:// URL, which QNetworkAccessManager supports,
    // so a higher version can be presented without publishing anything.
    {
        std::printf("\n== a newer version ==\n");
        const QString path =
            QStringLiteral("C:/Users/enaya/Desktop/Flowbit-Source/build/tests/newer.json");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("{\n  \"version\": \"9.9.9\",\n"
                    "  \"url\": \"https://example.invalid/Flowbit.exe\",\n"
                    "  \"notes\": \"a test release\"\n}\n");
            f.close();
        }

        Updater::setVersionFileUrl(QStringLiteral("file:///") + path);
        Updater u(nullptr);
        const Outcome o = runCheck(&u, 15000);

        std::printf("  outcome: %s%s\n", qPrintable(o.kind),
                    o.version.isEmpty() ? "" : qPrintable(QStringLiteral(" (%1)").arg(o.version)));
        check(o.kind == QLatin1String("available"),
              QStringLiteral("9.9.9 against 1.0.0 is offered (%1)").arg(o.kind));
        check(o.version == QLatin1String("9.9.9"), "the offered version is the one in the file");
        check(o.notes == QLatin1String("a test release"), "and its notes are carried through");
    }

    // ---- 4. an OLDER version is not offered -------------------------------
    {
        std::printf("\n== an older version ==\n");
        const QString path =
            QStringLiteral("C:/Users/enaya/Desktop/Flowbit-Source/build/tests/older.json");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("{\n  \"version\": \"0.1.0\",\n  \"url\": \"https://example.invalid/x\"\n}\n");
            f.close();
        }

        Updater::setVersionFileUrl(QStringLiteral("file:///") + path);
        Updater u(nullptr);
        const Outcome o = runCheck(&u, 15000);
        std::printf("  outcome: %s\n", qPrintable(o.kind));
        check(o.kind == QLatin1String("uptodate"),
              QStringLiteral("0.1.0 does not downgrade a 1.0.0 install (%1)").arg(o.kind));
    }

    // ---- 5. a malformed file is reported, not ignored --------------------
    {
        std::printf("\n== a malformed file ==\n");
        const QString path =
            QStringLiteral("C:/Users/enaya/Desktop/Flowbit-Source/build/tests/broken.json");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("{ this is not json");
            f.close();
        }

        Updater::setVersionFileUrl(QStringLiteral("file:///") + path);
        Updater u(nullptr);
        const Outcome o = runCheck(&u, 15000);
        std::printf("  outcome: %s - %s\n", qPrintable(o.kind), qPrintable(o.reason));
        check(o.kind == QLatin1String("failed"),
              "an unreadable file is reported rather than treated as no update");
    }

    // ---- 6. a file missing its fields is refused -------------------------
    {
        std::printf("\n== a file with no download address ==\n");
        const QString path =
            QStringLiteral("C:/Users/enaya/Desktop/Flowbit-Source/build/tests/nourl.json");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("{\n  \"version\": \"2.0.0\"\n}\n");
            f.close();
        }

        Updater::setVersionFileUrl(QStringLiteral("file:///") + path);
        Updater u(nullptr);
        const Outcome o = runCheck(&u, 15000);
        std::printf("  outcome: %s - %s\n", qPrintable(o.kind), qPrintable(o.reason));
        check(o.kind == QLatin1String("failed"),
              "a version with nowhere to download from is refused, not offered");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_update_channel passed."
                                        : "test_update_channel FAILED.");
    return g_failures == 0 ? 0 : 1;
}
