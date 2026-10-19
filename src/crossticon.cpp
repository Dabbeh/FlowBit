#include "crossticon.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace net {

QPixmap crossPixmap(int size, const QColor &colour, qreal strokeWidth, qreal scale)
{
    // Drawn at the device pixel ratio so it stays sharp on a scaled display. This project
    // targets a 125% screen, where a 1:1 pixmap would be resampled and look soft.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;

    QPixmap pm(QSize(int(size * dpr), int(size * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Two strokes meeting in the middle.
    //
    // A path with round caps and a join rather than a font glyph: the multiplication sign the
    // button used before (U+2715) is absent from Arial, which is the fallback this application
    // uses, so the button rendered as an empty box. A drawn cross depends on nothing.
    const qreal inset = (size - size * scale) / 2.0;
    const qreal a = inset;
    const qreal b = size - inset;

    QPen pen(colour, strokeWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);

    p.drawLine(QPointF(a, a), QPointF(b, b));
    p.drawLine(QPointF(b, a), QPointF(a, b));

    p.end();
    return pm;
}

QIcon crossIcon(int size, const QColor &colour, qreal strokeWidth, qreal scale)
{
    return QIcon(crossPixmap(size, colour, strokeWidth, scale));
}

} // namespace net
