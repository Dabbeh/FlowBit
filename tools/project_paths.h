#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

// Locating files in the project tree from a test.
//
// Several tests genuinely need the source as text rather than as code: they assert that a
// particular repair is still present, or that two places set the same string. That is a
// reasonable thing to check, but it needs the tree the test belongs to.
//
// Each of those tests used to hard-code one developer's Desktop, and to a directory name
// ("Flowbit-Source") that no longer exists. The file was therefore never found, and because
// the assertions were guarded by "if the source is readable", the checks silently degraded
// instead of failing - the worst outcome, since the test kept reporting success while
// verifying nothing at all.
//
// These helpers search from the places a test can reasonably be run from, so a test either
// finds the file or fails loudly.
namespace project {

// Absolute path to a project-relative file, or an empty string when it cannot be found.
//
// Searched, in order: the working directory and its parents (the tests run from
// build/tests), and the directory holding the test executable and its parents.
inline QString file(const QString &relative)
{
    const QString clean = QDir::fromNativeSeparators(relative);

    QStringList roots;
    roots << QDir::currentPath() << QCoreApplication::applicationDirPath();

    for (const QString &start : roots) {
        QDir dir(start);
        // Four levels is enough for build/tests/<exe> to reach the project root, with room
        // for a deeper out-of-source layout.
        for (int up = 0; up < 5; ++up) {
            const QString candidate = dir.filePath(clean);
            if (QFile::exists(candidate))
                return QDir::cleanPath(candidate);
            if (!dir.cdUp())
                break;
        }
    }
    return QString();
}

// Reads a project-relative file, or returns empty when it is missing. Callers should treat
// empty as a failure rather than skipping the checks that depend on it.
inline QString read(const QString &relative)
{
    const QString path = file(relative);
    if (path.isEmpty())
        return QString();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// A path beside the test executable, for scratch files.
//
// Used instead of a hard-coded build directory, which only ever existed on one machine.
inline QString scratch(const QString &name)
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

} // namespace project
