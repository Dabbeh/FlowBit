#include "settingsstore.h"

#include "schedule.h"

#include <QDebug>
#include <QDir>
#include <QStringList>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace net {

namespace {

QJsonObject ruleToJson(const Rule &rule)
{
    QJsonObject o;
    // Stored as a string: JSON numbers are doubles, and a quint64 id would lose
    // precision once it grew past 2^53. The id is opaque, so text is fine.
    o.insert(QStringLiteral("id"), QString::number(rule.id));
    o.insert(QStringLiteral("name"), rule.name);
    o.insert(QStringLiteral("from"), rule.from);
    o.insert(QStringLiteral("to"), rule.to);
    o.insert(QStringLiteral("downBps"), double(rule.down));
    o.insert(QStringLiteral("upBps"), double(rule.up));
    o.insert(QStringLiteral("days"), int(rule.days));
    o.insert(QStringLiteral("enabled"), rule.enabled);
    return o;
}

Rule ruleFromJson(const QJsonObject &o)
{
    Rule rule;
    // Absent for settings written before ids existed; ensureRuleIds() fills it in
    // after loading, which leaves existing user rules intact.
    rule.id = o.value(QStringLiteral("id")).toString().toULongLong();
    rule.name = o.value(QStringLiteral("name")).toString();
    rule.from = o.value(QStringLiteral("from")).toString(rule.from);
    rule.to = o.value(QStringLiteral("to")).toString(rule.to);
    rule.down = RateBps(o.value(QStringLiteral("downBps")).toDouble(0));
    rule.up = RateBps(o.value(QStringLiteral("upBps")).toDouble(0));
    rule.days = quint8(o.value(QStringLiteral("days")).toInt(0x7F));
    rule.enabled = o.value(QStringLiteral("enabled")).toBool(true);

    // Files written before the unit change stored kibibytes under downKBps/upKBps.
    if (o.contains(QStringLiteral("downKBps")))
        rule.down = RateBps(o.value(QStringLiteral("downKBps")).toDouble(0) * 1024.0);
    if (o.contains(QStringLiteral("upKBps")))
        rule.up = RateBps(o.value(QStringLiteral("upKBps")).toDouble(0) * 1024.0);

    if (rule.down < 0)
        rule.down = kUnlimited;
    if (rule.up < 0)
        rule.up = kUnlimited;
    if (rule.days == 0)
        rule.days = 0x7F;
    return rule;
}

QJsonObject appRuleToJson(const AppRule &rule)
{
    QJsonObject o;
    // Same reasoning as ruleToJson: the id is an opaque string so a quint64 cannot
    // lose precision through JSON's doubles.
    o.insert(QStringLiteral("id"), QString::number(rule.id));

    // The full path is the authoritative identity. The friendly name and the bare
    // executable name are stored alongside so the entry still displays correctly if
    // the file is later moved or renamed.
    o.insert(QStringLiteral("path"), rule.path);
    o.insert(QStringLiteral("name"), rule.name);
    o.insert(QStringLiteral("exe"), rule.exe);
    o.insert(QStringLiteral("downBps"), double(rule.down));
    o.insert(QStringLiteral("upBps"), double(rule.up));
    o.insert(QStringLiteral("enabled"), rule.enabled);

    // Per-application schedule windows reuse the global Rule shape, so they
    // serialise through the same helper and inherit overnight handling and day
    // masks rather than reimplementing them.
    QJsonArray windows;
    for (const Rule &w : rule.windows)
        windows.append(ruleToJson(w));
    o.insert(QStringLiteral("windows"), windows);

    return o;
}

AppRule appRuleFromJson(const QJsonObject &o)
{
    AppRule rule;
    rule.id = o.value(QStringLiteral("id")).toString().toULongLong();
    rule.path = o.value(QStringLiteral("path")).toString();
    rule.name = o.value(QStringLiteral("name")).toString();
    rule.exe = o.value(QStringLiteral("exe")).toString();
    rule.down = RateBps(o.value(QStringLiteral("downBps")).toDouble(0));
    rule.up = RateBps(o.value(QStringLiteral("upBps")).toDouble(0));
    rule.enabled = o.value(QStringLiteral("enabled")).toBool(true);

    const QJsonArray windows = o.value(QStringLiteral("windows")).toArray();
    for (const QJsonValue &v : windows) {
        if (v.isObject())
            rule.windows.append(ruleFromJson(v.toObject()));
    }

    if (rule.down < 0)
        rule.down = kUnlimited;
    if (rule.up < 0)
        rule.up = kUnlimited;

    // An entry whose exe name was never recorded still works through its path, so a
    // hand-edited or older file does not silently lose its application identity.
    if (rule.exe.isEmpty() && !rule.path.isEmpty())
        rule.exe = QFileInfo(rule.path).fileName();

    return rule;
}

} // namespace

namespace {

// The directory the pre-rebrand build used. Kept so existing settings and
// 30-day usage history survive the rename to Flowbit.
QString legacyConfigDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!base.isEmpty())
        return base + QStringLiteral("/NetValve");
    return QDir::homePath() + QStringLiteral("/.netvalve");
}

// The directory this application wrote to before the organisation name was dropped.
//
// Qt joins the organisation and the application into the config path, and both were set to
// "Flowbit", so everything lived one level deeper:
//
//     …/Local/Flowbit/Flowbit/settings.json     before
//     …/Local/Flowbit/settings.json             after
//
// Without this the application would see a location with no data, take the "fresh install"
// branch, and every existing setting, schedule and usage history would appear to have been
// wiped - the same trap the NetValve rename had, one level down.
QString doublyNestedConfigDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!base.isEmpty())
        return base + QStringLiteral("/Flowbit/Flowbit");
    return QDir::homePath() + QStringLiteral("/.flowbit/Flowbit");
}

bool directoryHasData(const QString &dir)
{
    if (dir.isEmpty() || !QDir(dir).exists())
        return false;

    if (QFile::exists(dir + QStringLiteral("/settings.json")))
        return true;
    // Any monthly usage history counts as data worth keeping.
    return !QDir(dir).entryList({QStringLiteral("usage-*.json")}, QDir::Files).isEmpty();
}

} // namespace

SettingsStore::SettingsStore()
{
    QString primary = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (primary.isEmpty())
        primary = QDir::homePath() + QStringLiteral("/.flowbit");

    // Most recent first: the pre-rebrand directory is the oldest, the nested one is what the
    // previous version of this application wrote.
    const QStringList older = {doublyNestedConfigDirectory(), legacyConfigDirectory()};

    // Prefer our own directory, but adopt an older one if that is where the data is, so an
    // upgrade never looks like a wiped install.
    //
    // The first older location that holds data wins. Checking in order matters: the nested
    // directory is the immediate predecessor, so it must be preferred over the NetValve one,
    // which is two renames back and would be stale.
    QString adopt;
    for (const QString &candidate : older) {
        if (candidate.isEmpty() || candidate == primary)
            continue;
        if (directoryHasData(candidate)) {
            adopt = candidate;
            break;
        }
    }

    if (!directoryHasData(primary) && !adopt.isEmpty()) {
        resolvePaths(adopt);
        m_usingLegacy = true;
    } else {
        resolvePaths(primary);
    }

    // The migration source: the location to copy from, which is whichever older directory still
    // holds data and is not the one now in use.
    //
    // The nested directory is preferred, but when the active directory IS the nested one - the
    // case where it held the data - the pre-rebrand one becomes the source instead, so a user
    // upgrading from the very first build still has their history carried across.
    for (const QString &candidate : older) {
        if (candidate != m_dir && directoryHasData(candidate)) {
            m_legacyDir = candidate;
            if (candidate == doublyNestedConfigDirectory())
                break;   // the immediate predecessor is the one worth taking from
        }
    }
}

SettingsStore::SettingsStore(const QString &directory)
{
    resolvePaths(directory);
}

void SettingsStore::resolvePaths(const QString &directory)
{
    m_dir = directory;
    while (m_dir.endsWith(QLatin1Char('/')) || m_dir.endsWith(QLatin1Char('\\')))
        m_dir.chop(1);
    m_path = m_dir + QStringLiteral("/settings.json");
}

int SettingsStore::migrateLegacyData() const
{
    if (m_legacyDir.isEmpty() || m_legacyDir == m_dir)
        return 0;

    if (!QDir().mkpath(m_dir))
        return 0;

    int copied = 0;
    const QStringList names =
        QDir(m_legacyDir).entryList({QStringLiteral("settings.json"), QStringLiteral("usage-*.json")},
                                    QDir::Files);

    for (const QString &name : names) {
        const QString from = m_legacyDir + QLatin1Char('/') + name;
        const QString to = m_dir + QLatin1Char('/') + name;

        const QFileInfo src(from);
        const QFileInfo dst(to);

        // Never clobber anything newer that already lives in the new location.
        if (dst.exists() && dst.lastModified() >= src.lastModified())
            continue;

        // The legacy files stay in place: if the user downgrades, the old build
        // must still find its data. This is a copy, not a move.
        if (QFile::copy(from, to)) {
            QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner);
            ++copied;
        }
    }
    return copied;
}

Settings SettingsStore::load() const
{
    Settings settings;

    QFile file(m_path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return settings;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return settings;

    const QJsonObject root = doc.object();
    settings.enabled = root.value(QStringLiteral("enabled")).toBool(false);
    settings.defaultDown = RateBps(root.value(QStringLiteral("defaultDownBps")).toDouble(0));
    settings.defaultUp = RateBps(root.value(QStringLiteral("defaultUpBps")).toDouble(0));
    settings.showThroughput = root.value(QStringLiteral("showThroughput")).toBool(true);

    // Startup and closing. The defaults match the struct, so a file written before these
    // existed behaves exactly as it did: nothing added to startup, X hides to the tray, and
    // update checks on.
    //
    // These were written by save() before they were read here, which is the worst version of
    // this mistake: the settings file recorded the user's choice and every launch silently
    // replaced it with the default, because loading never looked at the key.
    settings.startWithWindows =
        root.value(QStringLiteral("startWithWindows")).toBool(false);
    settings.closeToTray = root.value(QStringLiteral("closeToTray")).toBool(true);
    settings.checkForUpdates =
        root.value(QStringLiteral("checkForUpdates")).toBool(true);
    settings.skippedVersion =
        root.value(QStringLiteral("skippedVersion")).toString();
    settings.trayHintShown = root.value(QStringLiteral("trayHintShown")).toBool(false);

    // Appearance. Unknown or missing values fall back to Auto, so an older
    // settings file simply follows the OS.
    const QString themeName = root.value(QStringLiteral("themeMode")).toString();
    if (themeName == QLatin1String("light"))
        settings.themeMode = Settings::ThemeMode::Light;
    else if (themeName == QLatin1String("dark"))
        settings.themeMode = Settings::ThemeMode::Dark;
    else
        settings.themeMode = Settings::ThemeMode::Auto;

    // Legacy keys held kibibytes per second.
    if (root.contains(QStringLiteral("defaultDownKBps")))
        settings.defaultDown =
            RateBps(root.value(QStringLiteral("defaultDownKBps")).toDouble(0) * 1024.0);
    if (root.contains(QStringLiteral("defaultUpKBps")))
        settings.defaultUp =
            RateBps(root.value(QStringLiteral("defaultUpKBps")).toDouble(0) * 1024.0);

    if (settings.defaultDown < 0)
        settings.defaultDown = kUnlimited;
    if (settings.defaultUp < 0)
        settings.defaultUp = kUnlimited;

    const QJsonArray rules = root.value(QStringLiteral("rules")).toArray();
    settings.rules.reserve(rules.size());
    for (const QJsonValue &v : rules) {
        if (v.isObject())
            settings.rules.append(ruleFromJson(v.toObject()));
    }

    // Per-application limits. Absent in files written before the feature existed,
    // which simply leaves the list empty - no user data is affected either way.
    const QJsonArray appRules = root.value(QStringLiteral("appRules")).toArray();
    settings.appRules.reserve(appRules.size());
    for (const QJsonValue &v : appRules) {
        if (v.isObject())
            settings.appRules.append(appRuleFromJson(v.toObject()));
    }

    if (root.contains(QStringLiteral("usage"))) {
        const QJsonObject u = root.value(QStringLiteral("usage")).toObject();
        settings.usage.quotaBytes =
            u.value(QStringLiteral("quotaBytes")).toDouble(settings.usage.quotaBytes);
        settings.usage.cycle =
            u.value(QStringLiteral("cycle")).toString() == QLatin1String("monthly")
                ? CycleKind::Monthly
                : CycleKind::Rolling30;
        const QDate start =
            QDate::fromString(u.value(QStringLiteral("cycleStart")).toString(), Qt::ISODate);
        if (start.isValid())
            settings.usage.cycleStart = start;
    }

    // Settings written before rule ids existed come back with id == 0. Assign
    // ids in memory so every rule is addressable immediately; the next save
    // persists them. Nothing about the user's rules is otherwise altered.
    ensureRuleIds(settings);

    return settings;
}

bool SettingsStore::save(const Settings &settings, QString *error) const
{
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error)
            *error = QStringLiteral("Cannot create %1").arg(info.absolutePath());
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("enabled"), settings.enabled);
    root.insert(QStringLiteral("defaultDownBps"), double(settings.defaultDown));
    root.insert(QStringLiteral("defaultUpBps"), double(settings.defaultUp));
    root.insert(QStringLiteral("showThroughput"), settings.showThroughput);

    // Startup and closing. Written unconditionally rather than only when set, so the file
    // always states the behaviour plainly and a reader does not have to know the defaults.
    root.insert(QStringLiteral("startWithWindows"), settings.startWithWindows);
    root.insert(QStringLiteral("closeToTray"), settings.closeToTray);
    root.insert(QStringLiteral("checkForUpdates"), settings.checkForUpdates);
    if (!settings.skippedVersion.isEmpty())
        root.insert(QStringLiteral("skippedVersion"), settings.skippedVersion);

    // Written only when true: a false value is the default, and an older Flowbit reading this
    // file ignores the key rather than failing to parse it.
    if (settings.trayHintShown)
        root.insert(QStringLiteral("trayHintShown"), true);
    root.insert(QStringLiteral("themeMode"),
                settings.themeMode == Settings::ThemeMode::Light
                    ? QStringLiteral("light")
                    : (settings.themeMode == Settings::ThemeMode::Dark
                           ? QStringLiteral("dark")
                           : QStringLiteral("auto")));

    QJsonArray rules;
    for (const Rule &rule : settings.rules)
        rules.append(ruleToJson(rule));
    root.insert(QStringLiteral("rules"), rules);

    // Per-application limits. Written as a separate key so a build without the
    // feature, or an older Flowbit, ignores it rather than failing to parse.
    QJsonArray appRules;
    for (const AppRule &rule : settings.appRules)
        appRules.append(appRuleToJson(rule));
    root.insert(QStringLiteral("appRules"), appRules);

    QJsonObject usage;
    usage.insert(QStringLiteral("quotaBytes"), settings.usage.quotaBytes);
    usage.insert(QStringLiteral("cycle"),
                 settings.usage.cycle == CycleKind::Monthly ? QStringLiteral("monthly")
                                                            : QStringLiteral("rolling30"));
    usage.insert(QStringLiteral("cycleStart"),
                 settings.usage.cycleStart.toString(Qt::ISODate));
    root.insert(QStringLiteral("usage"), usage);

    // QSaveFile writes to a temp file and renames, so a crash mid-write cannot
    // leave a truncated settings file behind.
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
QString SettingsStore::describeContents(const Settings &settings)
{
    // Names the counts, so a confirmation can say what is about to be lost rather than warning
    // in the abstract. "This cannot be undone" is easy to click past; "3 schedules and 5
    // application rules" is not.
    const int schedules = settings.rules.size();
    const int apps = settings.appRules.size();

    int windows = 0;
    for (const AppRule &a : settings.appRules)
        windows += a.windows.size();

    QStringList parts;
    if (schedules > 0)
        parts << QStringLiteral("%1 schedule%2").arg(schedules).arg(schedules == 1 ? "" : "s");
    if (apps > 0)
        parts << QStringLiteral("%1 application rule%2").arg(apps).arg(apps == 1 ? "" : "s");
    if (windows > 0)
        parts << QStringLiteral("%1 app schedule%2").arg(windows).arg(windows == 1 ? "" : "s");

    if (parts.isEmpty())
        return QStringLiteral("No schedules or application rules are set up.");
    return QStringLiteral("This will remove %1.").arg(parts.join(QStringLiteral(", ")));
}

bool SettingsStore::resetEverything(QString *error)
{
    QStringList failures;

    // Enumerate and empty the directory rather than deleting a known list of file names.
    //
    // A reset that misses a file is worse than no reset, because the user believes it worked
    // and then finds the old state still there. Enumerating means a file added by a later
    // version is cleared by this one too.
    const QString dir = m_dir;
    if (!dir.isEmpty() && QDir(dir).exists()) {
        const QDir d(dir);
        const QFileInfoList entries =
            d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);

        for (const QFileInfo &fi : entries) {
            const QString path = fi.absoluteFilePath();
            bool removed = false;
            if (fi.isDir())
                removed = QDir(path).removeRecursively();
            else
                removed = QFile::remove(path);

            if (!removed)
                failures << path;
        }
    }

    // The previous brand's directory goes too.
    //
    // It is the source the one-time migration copies from, so leaving it would mean the next
    // launch restored the very data just cleared - a reset that undoes itself. It is only
    // touched when it holds data, and then only after the user has confirmed a full reset.
    const QString legacy = m_legacyDir;
    if (!legacy.isEmpty() && legacy != dir && QDir(legacy).exists()) {
        if (!QDir(legacy).removeRecursively()) {
            // Not a failure worth reporting on its own: the active directory is what the
            // application reads. Worth recording so the flag below is honest.
            qWarning("Flowbit: the previous brand's data directory could not be removed");
        }
    }

    if (!failures.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Could not remove: %1")
                         .arg(failures.join(QStringLiteral(", ")));
        }
        return false;
    }

    // The settings file is deliberately not recreated here. The caller saves the defaults it
    // just reset to, which means one write rather than a delete followed by a write - and the
    // application is running with those defaults already.
    return true;
}

} // namespace net
