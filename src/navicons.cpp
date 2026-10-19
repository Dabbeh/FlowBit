#include "navicons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace net {
namespace icons {

namespace {

// Everything is drawn on a 24x24 grid and scaled to the requested rect, so the
// stroke weight stays proportional at every size.
constexpr qreal kGrid = 24.0;

// One stroke width for all six icons. This is what makes the set look like a set
// rather than six unrelated marks.
constexpr qreal kStroke = 1.9;

QPen strokePen(const QColor &colour)
{
    QPen pen(colour, kStroke);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

void drawDashboard(QPainter &p)
{
    // A speedometer needle over a dial.
    //
    // First attempt drew an open arc plus a needle and read as a random squiggle
    // at 18px, so the dial is now a closed ring with a single bold needle pointing
    // up-right and no hub dot - fewer elements, clearer silhouette.
    p.drawEllipse(QRectF(4.5, 4.5, 15.0, 15.0));

    // Needle from the centre out to the upper right, with a slight taper so the
    // direction is obvious even at 16px.
    QPen needle = p.pen();
    needle.setWidthF(kStroke * 1.25);
    p.setPen(needle);
    p.drawLine(QPointF(12.0, 12.0), QPointF(17.0, 7.0));
}

void drawLimiter(QPainter &p)
{
    // A slider track with a handle: the manual limit.
    p.drawLine(QPointF(4.0, 12.0), QPointF(20.0, 12.0));
    p.setBrush(p.pen().color());
    p.drawEllipse(QPointF(14.5, 12.0), 3.1, 3.1);
    p.setBrush(Qt::NoBrush);
}

void drawScheduler(QPainter &p)
{
    // A clock. The first attempt had short hands that made the whole thing read as
    // a letter "Q", so the hands are now longer and clearly separated: one to 12,
    // one to 4.
    p.drawEllipse(QRectF(4.0, 4.0, 16.0, 16.0));

    QPen hand = p.pen();
    hand.setWidthF(kStroke);
    p.setPen(hand);
    p.drawLine(QPointF(12.0, 12.0), QPointF(12.0, 6.4));    // to 12 o'clock
    p.drawLine(QPointF(12.0, 12.0), QPointF(16.2, 14.6));   // to about 4 o'clock
}

void drawAppLimiter(QPainter &p)
{
    // A window with a bar inside: one application being limited.
    p.drawRoundedRect(QRectF(4.0, 5.0, 16.0, 14.0), 2.2, 2.2);
    p.drawLine(QPointF(4.0, 9.0), QPointF(20.0, 9.0));       // title bar
    p.drawLine(QPointF(7.5, 14.5), QPointF(16.5, 14.5));     // the limit bar
}

void drawStatistics(QPainter &p)
{
    // A bar chart: three columns of different heights on a baseline.
    p.drawLine(QPointF(4.0, 19.0), QPointF(20.0, 19.0));     // baseline
    p.drawLine(QPointF(7.5, 19.0), QPointF(7.5, 13.0));
    p.drawLine(QPointF(12.0, 19.0), QPointF(12.0, 8.5));
    p.drawLine(QPointF(16.5, 19.0), QPointF(16.5, 11.0));
}

void drawSettings(QPainter &p)
{
    // A gear: eight teeth around a ring. Drawn rather than typed, which is the
    // only way to guarantee it never falls back to a colour-emoji font.
    const QPointF centre(12.0, 12.0);
    const qreal outer = 9.4;
    const qreal inner = 7.0;
    const qreal tooth = 2.4;   // half-width of a tooth, in radians

    QPainterPath path;
    for (int i = 0; i < 8; ++i) {
        const qreal mid = i * (M_PI / 4.0);
        const qreal a0 = mid - tooth / 2.0;
        const qreal a1 = mid + tooth / 2.0;

        // Outer tooth edge
        const QPointF o0(centre.x() + outer * std::cos(a0),
                         centre.y() + outer * std::sin(a0));
        const QPointF o1(centre.x() + outer * std::cos(a1),
                         centre.y() + outer * std::sin(a1));
        // Where the tooth meets the body ring
        const QPointF b0(centre.x() + inner * std::cos(a0),
                         centre.y() + inner * std::sin(a0));
        const QPointF b1(centre.x() + inner * std::cos(a1),
                         centre.y() + inner * std::sin(a1));

        if (i == 0)
            path.moveTo(b0);
        else
            path.lineTo(b0);
        path.lineTo(o0);
        path.lineTo(o1);
        path.lineTo(b1);

        // Arc along the body to the next tooth
        const qreal nextMid = (i + 1) * (M_PI / 4.0);
        path.arcTo(QRectF(centre.x() - inner, centre.y() - inner,
                          inner * 2, inner * 2),
                   -i * 45.0 - 22.5, -45.0 + 22.5);
        Q_UNUSED(nextMid)
    }
    path.closeSubpath();
    p.drawPath(path);

    // The hub, left open like every other icon in the set.
    p.drawEllipse(centre, 3.0, 3.0);
}

} // namespace

void paint(QPainter &painter, Kind kind, const QRectF &rect, const QColor &colour)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Uniform scale from the 24x24 grid, and uniform stroke width, so an icon
    // drawn large is not bolder than one drawn small.
    const qreal scale = qMin(rect.width(), rect.height()) / kGrid;
    painter.translate(rect.center());
    painter.scale(scale, scale);
    painter.translate(-kGrid / 2.0, -kGrid / 2.0);

    QPen pen = strokePen(colour);
    pen.setWidthF(kStroke);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (kind) {
    case Kind::Dashboard:  drawDashboard(painter);  break;
    case Kind::Limiter:    drawLimiter(painter);    break;
    case Kind::Scheduler:  drawScheduler(painter);  break;
    case Kind::AppLimiter: drawAppLimiter(painter); break;
    case Kind::Statistics: drawStatistics(painter); break;
    case Kind::Settings:   drawSettings(painter);   break;
    }

    painter.restore();
}

QPixmap pixmap(Kind kind, int size, const QColor &colour, qreal dpr)
{
    const int px = qMax(1, int(size * dpr));
    QPixmap pm(px, px);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    paint(p, kind, QRectF(0, 0, px, px), colour);
    return pm;
}

QIcon icon(Kind kind, const QColor &colour)
{
    QIcon result;
    // Several sizes so Qt picks a crisp one for whichever widget asks.
    for (int s : {16, 18, 20, 24, 32, 48, 64}) {
        result.addPixmap(pixmap(kind, s, colour, 1.0));
        result.addPixmap(pixmap(kind, s, colour, 2.0));
    }
    return result;
}

} // namespace icons
} // namespace net
