#pragma once

#include <QIcon>
#include <QString>

namespace net {

// Maps an executable file name to a friendly application name.
//
// Only well-known names are mapped. An unrecognised executable keeps its own
// name, because inventing a friendly label from a partial match would mislabel
// unrelated programs.
QString friendlyAppName(const QString &executableName);

// Extracts the application's icon from its executable, caching the result.
//
// Extraction opens the file and walks its resources, which is far too costly to
// repeat once a second, so results are retained for the process lifetime. The
// cache is keyed by full path; a miss yields a neutral placeholder rather than a
// blank cell.
QIcon appIcon(const QString &executablePath, const QString &executableName);

// Drops cached icons for paths no longer in use, so a long session does not
// accumulate one icon per executable ever seen.
void pruneIconCache(const QSet<QString> &keepPaths);

// Number of cached icons, for tests and diagnostics.
int iconCacheSize();

} // namespace net
