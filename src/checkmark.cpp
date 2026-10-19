#include "checkmark.h"

#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QStandardPaths>

namespace net {
namespace theme {

namespace {

// The tick, as SVG. 18x18 to match the indicator, drawn with round caps and joins so it stays
// crisp when the display scales it.
//
// One definition, used both for the file and for the inline accessor, so the two cannot drift.
QByteArray tickSvg()
{
    // The tick sits ON the primary fill, so it has to contrast with that - not with the card.
    //
    // The palette's Text colour was the wrong choice and produced a white tick on a white
    // indicator, which is why the checked box rendered as a plain square. kPrimaryButtonText()
    // is the theme's existing answer to "what colour goes on a primary fill" - the same one the
    // palette already uses for HighlightedText - so the two cannot disagree.
    const QColor ink = kPrimaryButtonText();
    return QStringLiteral(
               "<svg xmlns='http://www.w3.org/2000/svg' width='18' height='18' "
               "viewBox='0 0 18 18'>"
               "<path d='M4.6 9.4 L7.4 12.2 L13.4 5.6' fill='none' stroke='%1' "
               "stroke-width='2.4' stroke-linecap='round' stroke-linejoin='round'/>"
               "</svg>")
        .arg(ink.name(QColor::HexRgb))
        .toUtf8();
}

} // namespace

QByteArray checkmarkSvg()
{
    return tickSvg();
}

QString generateCheckmark()
{
    const QByteArray svg = tickSvg();

    // Beside the settings file, which is the one directory this application is known to be able
    // to write to. The executable's own directory is deliberately not used: under Program Files
    // it needs elevation, and the tick must not depend on that.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        return QString();

    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("checkmark.svg"));

    // Rewritten only when it differs, so a launch does not churn the file. The contents change
    // when the theme does, which is exactly when a rewrite is wanted.
    QFile existing(path);
    if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
        const bool same = existing.readAll() == svg;
        existing.close();
        if (same)
            return path;
    }

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    out.write(svg);
    out.close();

    // Forward slashes: Qt accepts them on Windows and a backslash inside a stylesheet url()
    // would need escaping.
    return QDir::fromNativeSeparators(path);
}

} // namespace theme
} // namespace net
