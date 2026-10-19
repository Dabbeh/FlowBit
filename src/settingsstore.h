#pragma once

#include "settings.h"

#include <QString>

namespace net {

// Persists Settings as JSON, and owns the directory that also holds the usage
// history files.
//
// Rebranding note (Flowbit -> Flowbit): the on-disk location is deliberately
// NOT renamed outright. Existing installs have real data - settings.json plus
// monthly usage-YYYY-MM.json history driving the 30-day panel - under the old
// %LOCALAPPDATA%/Flowbit directory. resolvePaths() therefore prefers the new
// Flowbit directory but falls back to the legacy one, and migrateLegacyData()
// copies the old files forward on first run so nothing is lost either way.
class SettingsStore
{
public:
    SettingsStore();

    // Overrides the config directory. Used by tests to stay hermetic: on Windows
    // QStandardPaths resolves AppConfigLocation to LocalAppData rather than
    // Roaming, so an environment-variable override is not reliable.
    explicit SettingsStore(const QString &directory);

    // Absolute path of the settings file (also used to show it in the UI).
    QString filePath() const { return m_path; }

    // Directory holding settings.json and the usage-*.json history files.
    QString directory() const { return m_dir; }

    // Directory the previous brand used, when it exists and holds data. Empty
    // when there is nothing to inherit.
    QString legacyDirectory() const { return m_legacyDir; }
    bool isUsingLegacyDirectory() const { return m_usingLegacy; }

    // Test seam: declares where the previous brand kept its data, so the
    // migration can be exercised without touching the real user profile.
    void setLegacyDirectoryForTesting(const QString &dir) { m_legacyDir = dir; }

    // Copies settings.json and any usage-*.json from the legacy directory into
    // the active one, without overwriting newer files. Returns the number of
    // files copied. Safe to call repeatedly.
    int migrateLegacyData() const;

    // Returns default-constructed settings when the file is missing/unreadable.
    // Returns Flowbit to a fresh install: every settings file, the usage history and the
    // directory carried over from the previous brand are deleted.
    //
    // The whole directory is emptied rather than a known list of names, so a file added by a
    // later version is cleared by this one too. A reset that silently misses something is worse
    // than no reset, because the user believes it worked.
    //
    // The driver service, the installed files, the scheduled task and the Run key are NOT
    // touched here - the first two are not preferences, and the last two are in the registry
    // rather than on disk. The caller removes those.
    //
    // Returns false and fills `error` when a file could not be removed, so the caller can say
    // what happened rather than reporting a success that did not occur.
    bool resetEverything(QString *error = nullptr);

    // How many schedules and application rules the current settings hold, so a confirmation can
    // name what is about to be lost rather than warning in the abstract.
    static QString describeContents(const Settings &settings);

    Settings load() const;

    // Writes atomically (temp file + rename). Returns false and sets `error`
    // when the settings directory cannot be created or written.
    bool save(const Settings &settings, QString *error = nullptr) const;

private:
    void resolvePaths(const QString &directory);

    QString m_dir;
    QString m_path;
    QString m_legacyDir;      // previous brand's directory, if it has data
    bool m_usingLegacy = false;
};

} // namespace net
