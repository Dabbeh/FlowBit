#include "speedgraph.h"

#include "theme.h"
#include "units.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>

namespace net {

SpeedGraph::SpeedGraph(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setMinimumHeight(110);
    // Grows with the window instead of staying a fixed strip: the card already gives it stretch,
    // and the plot is drawn from rect() so a larger area is simply a larger graph.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SpeedGraph::addSample(double downBytesPerSec, double upBytesPerSec)
{
    m_down.append(std::max(0.0, downBytesPerSec));
    m_up.append(std::max(0.0, upBytesPerSec));

    while (m_down.size() > kCapacity)
        m_down.removeFirst();
    while (m_up.size() > kCapacity)
        m_up.removeFirst();

    update();
}

void SpeedGraph::setCeiling(RateBps downCap)
{
    if (m_ceiling == downCap)
        return;
    m_ceiling = downCap;
    update();
}

void SpeedGraph::clearHistory()
{
    m_down.clear();
    m_up.clear();
    update();
}

void SpeedGraph::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    // Graph bed: slightly sunken so the trace reads as sitting inside the card.
    QPainterPath bed;
    bed.addRoundedRect(bounds, 8, 8);
    p.fillPath(bed, theme::kPanelSunken());
    p.setPen(QPen(theme::kBorder(), 1));
    p.drawPath(bed);

    // Leave room on the right for rate labels.
    const QRectF plot = bounds.adjusted(8, 8, -46, -8);
    if (plot.width() < 10 || plot.height() < 10)
        return;

    // Scale to the largest sample in view, but never below the active ceiling -
    // otherwise the dashed cap line would sit off the top of the plot, which is
    // exactly when the user most wants to see it.
    double maxValue = 1.0;
    for (double v : m_down)
        maxValue = std::max(maxValue, v);
    for (double v : m_up)
        maxValue = std::max(maxValue, v);
    if (m_ceiling > 0)
        maxValue = std::max(maxValue, double(m_ceiling));
    maxValue *= 1.15;  // headroom so the peak is not glued to the top edge

    drawGrid(p, plot);
    drawMountain(p, plot, maxValue);
    drawAverages(p, plot, maxValue);
    drawCeiling(p, plot, maxValue);
    drawAxisLabels(p, plot, maxValue);
}

double SpeedGraph::averageDown() const
{
    if (m_down.isEmpty())
        return 0.0;
    double sum = 0.0;
    for (double v : m_down)
        sum += v;
    return sum / double(m_down.size());
}

double SpeedGraph::averageUp() const
{
    if (m_up.isEmpty())
        return 0.0;
    double sum = 0.0;
    for (double v : m_up)
        sum += v;
    return sum / double(m_up.size());
}

void SpeedGraph::drawAverages(QPainter &p, const QRectF &plot, double maxValue) const
{
    if (m_down.isEmpty())
        return;

    auto yFor = [&](double v) {
        const double frac = std::clamp(v / maxValue, 0.0, 1.0);
        return plot.bottom() - frac * plot.height();
    };

    struct Line {
        double value;
        QColor colour;
        QString label;
    };
    const Line lines[] = {
        {averageDown(), theme::kGraphDownload(), QStringLiteral("avg \u2193")},
        {averageUp(), theme::kGraphUpload(), QStringLiteral("avg \u2191")},
    };

    for (const Line &line : lines) {
        if (line.value <= 0.0)
            continue;
        const qreal y = yFor(line.value);

        QPen pen(line.colour, 1, Qt::DashLine);
        pen.setDashPattern({4, 3});
        p.setPen(pen);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));

        // Label inside the plot, at the left edge: the right edge belongs to the axis labels, and
        // overlapping them would be worse than a slightly cramped label.
        p.setPen(theme::kTextMuted());
        p.setFont(theme::uiFont(8, false));
        const QRectF labelRect(plot.left() + 4, y - 14, 120, 13);
        p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("%1 %2").arg(line.label, formatRate(line.value)));
    }
}

void SpeedGraph::drawGrid(QPainter &p, const QRectF &plot) const
{
    QPen pen(theme::kBorder(), 1, Qt::DotLine);
    pen.setDashPattern({2, 4});
    p.setPen(pen);

    // Four horizontal guides, plus vertical second markers every 15s.
    for (int i = 1; i < 4; ++i) {
        const qreal y = plot.top() + plot.height() * i / 4.0;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    for (int s = 15; s < kCapacity; s += 15) {
        const qreal x = plot.left() + plot.width() * s / double(kCapacity - 1);
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
}

void SpeedGraph::drawMountain(QPainter &p, const QRectF &plot, double maxValue) const
{
    if (m_down.isEmpty())
        return;

    // x positions are pinned to the right edge so the trace scrolls leftwards as
    // new samples arrive, even before the buffer is full.
    const int count = m_down.size();
    const double step = plot.width() / double(kCapacity - 1);
    const double x0 = plot.right() - step * (count - 1);

    auto yFor = [&](double v) {
        const double frac = std::clamp(v / maxValue, 0.0, 1.0);
        return plot.bottom() - frac * plot.height();
    };

    // -- download: filled area + line -------------------------------------
    QPainterPath area;
    area.moveTo(x0, plot.bottom());
    for (int i = 0; i < count; ++i)
        area.lineTo(x0 + step * i, yFor(m_down[i]));
    area.lineTo(x0 + step * (count - 1), plot.bottom());
    area.closeSubpath();

    QLinearGradient fill(0, plot.top(), 0, plot.bottom());
    QColor cyanTop = theme::kGraphDownload();
    cyanTop.setAlpha(150);
    QColor cyanBottom = theme::kGraphDownload();
    cyanBottom.setAlpha(12);
    fill.setColorAt(0.0, cyanTop);
    fill.setColorAt(1.0, cyanBottom);
    p.fillPath(area, fill);

    QPainterPath line;
    for (int i = 0; i < count; ++i) {
        const QPointF pt(x0 + step * i, yFor(m_down[i]));
        if (i == 0)
            line.moveTo(pt);
        else
            line.lineTo(pt);
    }
    p.setPen(QPen(theme::kGraphDownload(), 1.6));
    p.drawPath(line);

    // -- upload: orange line, no fill -------------------------------------
    if (!m_up.isEmpty()) {
        QPainterPath upLine;
        for (int i = 0; i < m_up.size(); ++i) {
            const QPointF pt(x0 + step * i, yFor(m_up[i]));
            if (i == 0)
                upLine.moveTo(pt);
            else
                upLine.lineTo(pt);
        }
        p.setPen(QPen(theme::kGraphUpload(), 1.3));
        p.drawPath(upLine);
    }
}

void SpeedGraph::drawCeiling(QPainter &p, const QRectF &plot, double maxValue) const
{
    if (m_ceiling <= 0)
        return;

    const double frac = std::clamp(double(m_ceiling) / maxValue, 0.0, 1.0);
    const qreal y = plot.bottom() - frac * plot.height();

    QPen pen(theme::kGraphDownload(), 1.0, Qt::DashLine);
    pen.setDashPattern({5, 4});
    p.setPen(pen);
    p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
}

void SpeedGraph::drawAxisLabels(QPainter &p, const QRectF &plot, double maxValue) const
{
    QFont f = theme::uiFont(7);
    p.setFont(f);
    p.setPen(theme::kTextMuted());

    const QFontMetrics fm(f);
    const int rightX = int(plot.right()) + 6;

    // Top label = axis maximum, bottom label = zero.
    const QString top = formatRate(RateBps(maxValue / 1.15));
    p.drawText(QRect(rightX, int(plot.top()) - fm.height() / 2, 44, fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter, top);
    p.drawText(QRect(rightX, int(plot.bottom()) - fm.height() / 2, 44, fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"));
}

} // namespace net
