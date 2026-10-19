#include "brandmark.h"

#include "theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSvgRenderer>

namespace net {
namespace brand {

namespace {

QString g_svgPath;
bool g_vectorOk = false;
bool g_probed = false;

// The two supplied variants differ only in plate colour and gradient tuning:
// the dark one sits on a navy plate, the light one on a near-white plate.
QString darkSvgPath()
{
    if (!g_svgPath.isEmpty())
        return g_svgPath;

    // Search beside the executable first (a normal install), then the source
    // tree (a dev build), so the vector is found in both layouts.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/flowbit-dark.svg"),
        appDir + QStringLiteral("/../docs/flowbit-dark.svg"),
        appDir + QStringLiteral("/../../docs/flowbit-dark.svg"),
        appDir + QStringLiteral("/../../../docs/flowbit-dark.svg"),
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return c;
    }
    return QString();
}

QString lightSvgPath()
{
    QString p = darkSvgPath();
    if (p.isEmpty())
        return QString();
    p.replace(QStringLiteral("flowbit-dark.svg"), QStringLiteral("flowbit-light.svg"));
    return QFileInfo::exists(p) ? p : QString();
}

// Last-resort painted mark, used only if neither SVG can be found. It follows the
// same two-ribbon construction as the real logo rather than a letterform, so a
// missing asset degrades to something recognisable instead of something wrong.
void paintFallbackRibbons(QPainter &p, qreal s)
{
    auto scale = [s](qreal v) { return v * s / 512.0; };

    // Upper ribbon.
    QPainterPath upper;
    upper.moveTo(scale(124), scale(305));
    upper.cubicTo(scale(126), scale(226), scale(146), scale(158), scale(211), scale(137));
    upper.cubicTo(scale(243), scale(127), scale(278), scale(133), scale(329), scale(132));
    upper.cubicTo(scale(367), scale(132), scale(393), scale(120), scale(416), scale(104));
    upper.cubicTo(scale(417), scale(158), scale(394), scale(196), scale(346), scale(202));
    upper.cubicTo(scale(307), scale(207), scale(260), scale(199), scale(222), scale(210));
    upper.cubicTo(scale(174), scale(224), scale(144), scale(254), scale(124), scale(305));
    upper.closeSubpath();

    QLinearGradient ug(scale(124), scale(104), scale(416), scale(305));
    ug.setColorAt(0.0, QColor(0x22, 0xD3, 0xEE));
    ug.setColorAt(1.0, QColor(0x06, 0xB6, 0xD4));
    p.fillPath(upper, ug);

    // Lower ribbon.
    QPainterPath lower;
    lower.moveTo(scale(114), scale(401));
    lower.cubicTo(scale(112), scale(339), scale(134), scale(280), scale(177), scale(252));
    lower.cubicTo(scale(206), scale(233), scale(237), scale(230), scale(270), scale(230));
    lower.lineTo(scale(335), scale(230));
    lower.cubicTo(scale(339), scale(270), scale(311), scale(297), scale(277), scale(302));
    lower.cubicTo(scale(245), scale(307), scale(216), scale(298), scale(194), scale(318));
    lower.cubicTo(scale(171), scale(339), scale(153), scale(386), scale(114), scale(401));
    lower.closeSubpath();

    QLinearGradient lg(scale(114), scale(230), scale(335), scale(401));
    lg.setColorAt(0.0, QColor(0x06, 0xB6, 0xD4));
    lg.setColorAt(1.0, QColor(0x3B, 0x82, 0xF6));
    p.fillPath(lower, lg);
}

} // namespace

void setSvgPath(const QString &path)
{
    g_svgPath = path;
    g_probed = false;
}

bool usingVectorSource()
{
    if (!g_probed) {
        const QString p = darkSvgPath();
        if (!p.isEmpty()) {
            QSvgRenderer r(p);
            g_vectorOk = r.isValid();
        }
        g_probed = true;
    }
    return g_vectorOk;
}

QIcon mark(int size, bool slash, bool plateForDarkTheme)
{
    constexpr int kRender = 256;   // render large, let Qt scale for any context
    QPixmap pm(kRender, kRender);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QString svg = plateForDarkTheme ? darkSvgPath() : lightSvgPath();
    // An empty path makes QSvgRenderer log "Cannot open file ''" once per call,
    // four warnings per session. Guard it: the painted fallback below is the whole
    // point of having it.
    QSvgRenderer renderer;
    if (!svg.isEmpty())
        renderer.load(svg);
    if (renderer.isValid()) {
        renderer.render(&p);
        g_vectorOk = true;
        g_probed = true;
    } else {
        // Fallback: navy plate plus the same two ribbons.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x0F, 0x17, 0x2A));
        p.drawRoundedRect(QRectF(0, 0, kRender, kRender), kRender * 0.22, kRender * 0.22);
        paintFallbackRibbons(p, kRender);
        g_vectorOk = false;
        g_probed = true;
    }

    if (slash) {
        // Red diagonal indicating limiting is off. Kept thick and short of the
        // corners so it reads clearly even at tray size.
        QPen pen(QColor(0xE7, 0x4C, 0x3C), kRender * 0.10, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(kRender * 0.20, kRender * 0.80),
                   QPointF(kRender * 0.80, kRender * 0.20));
    }
    p.end();

    QIcon icon;
    icon.addPixmap(pm);
    Q_UNUSED(size)
    return icon;
}

} // namespace brand
} // namespace net
