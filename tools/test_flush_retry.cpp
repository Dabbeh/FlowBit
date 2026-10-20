// Verifies that a failed history write is retried, not forgotten.
//
// flush() used to clear the dirty-month sets unconditionally after attempting the writes. A failed
// write therefore dropped the month from the queue forever - the data was not merely unsaved, it was
// never retried, and the 30-day view silently came back short.

#include "usagetracker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace net;

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok) ++g_failures;
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("\n== a failed write is retried ==\n");

    QTemporaryDir tmp;
    // Point the tracker at a path that is a FILE, so mkpath and the writes both fail.
    const QString blocked = tmp.path() + "/blocked";
    {
        QFile f(blocked);
        check(f.open(QIODevice::WriteOnly), "a file is created where the directory should be");
        f.write("x");
    }

    UsageTracker t;
    t.setDataDirectory(blocked);
    t.addSample(1024 * 1024, 4096);

    const int firstAttempt = t.flush();
    std::printf("  failing directory: flush reported %d failed write(s)\n", firstAttempt);
    check(firstAttempt > 0, "a write that cannot happen is reported");

    const int secondAttempt = t.flush();
    std::printf("  retry:             flush reported %d failed write(s)\n", secondAttempt);
    check(secondAttempt > 0, "and it is retried on the next flush instead of being forgotten");

    // Now make the directory usable and confirm the queued data lands.
    QFile::remove(blocked);
    const int afterFix = t.flush();
    std::printf("  after the path is usable: %d failed write(s)\n", afterFix);
    check(afterFix == 0, "once the path works, the queued month is written");
    check(QDir(blocked).exists(), "the directory now exists");
    const QStringList written = QDir(blocked).entryList({"usage-*.json"}, QDir::Files);
    std::printf("  written: %s\n", qPrintable(written.join(", ")));
    check(!written.isEmpty(), "and the history file is there");

    std::printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
                g_failures == 0 ? "test_flush_retry passed." : "FAILED.");
    return g_failures == 0 ? 0 : 1;
}
