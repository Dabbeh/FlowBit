#include "appnames.h"

#include "theme.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSet>

#include <windows.h>
#include <shellapi.h>

namespace net {

namespace {

// Well-known executables. Deliberately a fixed list: a heuristic that guessed
// from a substring would label unrelated binaries as, say, Chrome.
const QHash<QString, QString> &knownApps()
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("chrome.exe"), QStringLiteral("Google Chrome")},
        {QStringLiteral("msedge.exe"), QStringLiteral("Microsoft Edge")},
        {QStringLiteral("firefox.exe"), QStringLiteral("Mozilla Firefox")},
        {QStringLiteral("brave.exe"), QStringLiteral("Brave")},
        {QStringLiteral("opera.exe"), QStringLiteral("Opera")},
        {QStringLiteral("steam.exe"), QStringLiteral("Steam")},
        {QStringLiteral("steamwebhelper.exe"), QStringLiteral("Steam")},
        {QStringLiteral("discord.exe"), QStringLiteral("Discord")},
        {QStringLiteral("spotify.exe"), QStringLiteral("Spotify")},
        {QStringLiteral("slack.exe"), QStringLiteral("Slack")},
        {QStringLiteral("teams.exe"), QStringLiteral("Microsoft Teams")},
        {QStringLiteral("ms-teams.exe"), QStringLiteral("Microsoft Teams")},
        {QStringLiteral("zoom.exe"), QStringLiteral("Zoom")},
        {QStringLiteral("telegram.exe"), QStringLiteral("Telegram")},
        {QStringLiteral("whatsapp.exe"), QStringLiteral("WhatsApp")},
        {QStringLiteral("code.exe"), QStringLiteral("Visual Studio Code")},
        {QStringLiteral("devenv.exe"), QStringLiteral("Visual Studio")},
        {QStringLiteral("explorer.exe"), QStringLiteral("Windows Explorer")},
        {QStringLiteral("onedrive.exe"), QStringLiteral("OneDrive")},
        {QStringLiteral("dropbox.exe"), QStringLiteral("Dropbox")},
        {QStringLiteral("svchost.exe"), QStringLiteral("Windows Service Host")},
        {QStringLiteral("system"), QStringLiteral("System")},
        {QStringLiteral("tiworker.exe"), QStringLiteral("Windows Update")},
        {QStringLiteral("usoclient.exe"), QStringLiteral("Windows Update")},
        {QStringLiteral("wuauclt.exe"), QStringLiteral("Windows Update")},
        {QStringLiteral("bits.exe"), QStringLiteral("Background Transfer")},
        {QStringLiteral("flowbit.exe"), QStringLiteral("Flowbit")},
        {QStringLiteral("qbittorrent.exe"), QStringLiteral("qBittorrent")},
        {QStringLiteral("utorrent.exe"), QStringLiteral("uTorrent")},
        {QStringLiteral("thunderbird.exe"), QStringLiteral("Thunderbird")},
        {QStringLiteral("outlook.exe"), QStringLiteral("Microsoft Outlook")},
        {QStringLiteral("nvidia share.exe"), QStringLiteral("NVIDIA Share")},
        {QStringLiteral("epicgameslauncher.exe"), QStringLiteral("Epic Games Launcher")},
        {QStringLiteral("goggalaxy.exe"), QStringLiteral("GOG Galaxy")},
        {QStringLiteral("battle.net.exe"), QStringLiteral("Battle.net")},
        {QStringLiteral("origin.exe"), QStringLiteral("EA App")},
        {QStringLiteral("eadesktop.exe"), QStringLiteral("EA App")},
        {QStringLiteral("ubisoftconnect.exe"), QStringLiteral("Ubisoft Connect")},
        {QStringLiteral("curl.exe"), QStringLiteral("curl")},
        {QStringLiteral("git.exe"), QStringLiteral("Git")},
    };
    return table;
}

QHash<QString, QIcon> &iconCache()
{
    static QHash<QString, QIcon> cache;
    return cache;
}

// A neutral square with the first letter of the application, used when no icon
// can be extracted. Drawn in theme colours so it matches the surrounding UI.
QIcon placeholderIcon(const QString &executableName)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kPanelSunken());
    p.drawRoundedRect(QRectF(1, 1, 30, 30), 7, 7);
    p.setPen(QPen(theme::kBorder(), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(1, 1, 30, 30), 7, 7);

    QString letter = executableName.left(1).toUpper();
    if (letter.isEmpty())
        letter = QStringLiteral("?");

    QFont f = theme::uiFont(13, true);
    p.setFont(f);
    p.setPen(theme::kTextMuted());
    p.drawText(QRectF(1, 1, 30, 30), Qt::AlignCenter, letter);
    p.end();

    return QIcon(pm);
}

} // namespace

QString friendlyAppName(const QString &executableName)
{
    const QString key = executableName.toLower();
    const auto it = knownApps().constFind(key);
    if (it != knownApps().constEnd())
        return it.value();

    // Unrecognised: show the executable name with the extension removed, which is
    // honest and still readable.
    if (key.endsWith(QLatin1String(".exe")))
        return executableName.left(executableName.length() - 4);
    return executableName;
}

QIcon appIcon(const QString &executablePath, const QString &executableName)
{
    if (executablePath.isEmpty())
        return placeholderIcon(executableName);

    {
        const auto it = iconCache().constFind(executablePath);
        if (it != iconCache().constEnd())
            return it.value();
    }

    QIcon icon;

    // Ask the shell for the large icon, which already handles the executable's
    // own resource lookup, shortcut overlays and DPI.
    SHFILEINFOW info{};
    const UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
    const QString native = QDir::toNativeSeparators(executablePath);
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(native.utf16()), 0, &info, sizeof(info), flags)
        && info.hIcon) {
        icon = QIcon(QPixmap::fromImage(QImage::fromHICON(info.hIcon)));
        DestroyIcon(info.hIcon);
    }

    if (icon.isNull())
        icon = placeholderIcon(executableName);

    iconCache().insert(executablePath, icon);
    return icon;
}

void pruneIconCache(const QSet<QString> &keepPaths)
{
    for (auto it = iconCache().begin(); it != iconCache().end();) {
        if (keepPaths.contains(it.key()))
            ++it;
        else
            it = iconCache().erase(it);
    }
}

int iconCacheSize()
{
    return iconCache().size();
}

} // namespace net
