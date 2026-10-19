// Verifies the update version comparison.
//
// This is the one piece of the updater where a mistake is invisible: if isNewer() is
// wrong, no update is ever offered, nothing errors, and the application simply stays on an
// old version. A string comparison is the natural mistake - "1.10.0" sorts before "1.9.0"
// because '1' precedes '9' - so those cases are checked explicitly.
//
// The network half of the updater is not exercised here: fetching a URL and swapping an
// executable cannot be asserted without a server and a second copy of the program, and
// pretending otherwise would be a test that proves nothing.

#include "updater.h"

#include <QCoreApplication>

#include <cstdio>

using namespace net;

namespace {

int g_checks = 0;
int g_failures = 0;

void expectNewer(const QString &candidate, bool wanted, const char *why)
{
    ++g_checks;
    const bool got = Updater::isNewer(candidate);
    const bool ok = (got == wanted);
    std::printf("  %s  %-10s vs running %-8s -> %-5s   %s\n", ok ? "ok  " : "FAIL",
                qPrintable(candidate), qPrintable(Updater::currentVersion()),
                got ? "newer" : "not newer", why);
    if (!ok)
        ++g_failures;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // Pin the running version so the expectations are readable rather than depending on
    // whatever the build happens to carry.
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    std::printf("\n== version comparison, running %s ==\n",
                qPrintable(Updater::currentVersion()));

    expectNewer(QStringLiteral("1.0.1"), true, "a patch release");
    expectNewer(QStringLiteral("1.1.0"), true, "a minor release");
    expectNewer(QStringLiteral("2.0.0"), true, "a major release");

    expectNewer(QStringLiteral("1.0.0"), false, "the same version");
    expectNewer(QStringLiteral("0.9.9"), false, "an older version");

    // The case a string comparison gets wrong.
    QCoreApplication::setApplicationVersion(QStringLiteral("1.9.0"));
    std::printf("\n== with 1.9.0 running ==\n");
    expectNewer(QStringLiteral("1.10.0"), true,
                "1.10 IS newer than 1.9, though it sorts before it as text");
    expectNewer(QStringLiteral("1.9.1"), true, "a patch on the same minor");
    expectNewer(QStringLiteral("1.8.9"), false, "an older minor");

    // Uneven lengths must not read as older.
    QCoreApplication::setApplicationVersion(QStringLiteral("1.2"));
    std::printf("\n== with 1.2 running ==\n");
    expectNewer(QStringLiteral("1.2.0"), false, "1.2.0 is the same as 1.2, not newer");
    expectNewer(QStringLiteral("1.2.1"), true, "1.2.1 is newer than 1.2");

    // A suffix must not turn the comparison into nonsense.
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    std::printf("\n== suffixes ==\n");
    expectNewer(QStringLiteral("1.1.0-beta"), true, "a beta of a newer version still counts");
    expectNewer(QStringLiteral("1.0.0-beta"), false, "a beta of the same version is not newer");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_updater passed." : "test_updater FAILED.");
    return g_failures == 0 ? 0 : 1;
}
