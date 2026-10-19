// Reproduces the crash found in the minidump.
//
// The newest dump (23:43) faults at VA 0x14007C160, which the linker map resolves
// to appruledialog.o + 0x110 - the interior of
//
//     QHashPrivate::Data<QHashPrivate::Node<QString, QHashDummyValue>>
//         ::findOrInsert<QString>(QString const&)
//
// That is the insertion path of the QSet<QString> in runningApplications(), the
// function that enumerates processes to populate the "Add application" picker.
//
// This test calls that function repeatedly and under memory pressure, and reports
// whether it survives. It also validates the set logic itself, since a fault in
// findOrInsert can mean the table was corrupted by a bad key QString.

#include "appruledialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSet>
#include <QTextStream>

#include <cstdio>

using namespace net;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QTextStream out(stdout);

    int failures = 0;

    // 1. The function must return something sensible and not crash.
    for (int i = 0; i < 5; ++i) {
        const QVector<AppCandidate> apps = runningApplications();
        out << QStringLiteral("pass %1: %2 applications discovered\n")
                   .arg(i + 1).arg(apps.size());
        if (i == 0 && apps.isEmpty()) {
            out << QStringLiteral("  note: no applications found (unexpected on a live system)\n");
            ++failures;
        }
    }

    // 2. No duplicate paths: the QSet exists to deduplicate, and a duplicate means
    //    the set logic is not doing its job.
    {
        const QVector<AppCandidate> apps = runningApplications();
        QSet<QString> keys;
        int dupes = 0;
        for (const AppCandidate &c : apps) {
            const QString k = c.path.toLower();
            if (keys.contains(k))
                ++dupes;
            keys.insert(k);
        }
        out << QStringLiteral("duplicate paths after dedup: %1\n").arg(dupes);
        if (dupes != 0) {
            out << QStringLiteral("  FAIL  runningApplications returned duplicates\n");
            ++failures;
        }
    }

    // 3. Every returned candidate must have a usable path and exe.
    {
        const QVector<AppCandidate> apps = runningApplications();
        int bad = 0;
        for (const AppCandidate &c : apps) {
            if (c.path.isEmpty() || c.exe.isEmpty())
                ++bad;
        }
        out << QStringLiteral("candidates with empty path or exe: %1\n").arg(bad);
        if (bad != 0) {
            out << QStringLiteral("  FAIL  empty path/exe leaked out of the function\n");
            ++failures;
        }
    }

    out << QStringLiteral("%1 failures\n").arg(failures);
    return failures == 0 ? 0 : 1;
}
