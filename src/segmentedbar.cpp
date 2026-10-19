#include "segmentedbar.h"

#include "theme.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace net {

SegmentedBar::SegmentedBar(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(8);
}

void SegmentedBar::setSegments(int segments)
{
    m_segments = std::max(1, segments);
    update();
}

void SegmentedBar::setProgress(double fraction)
{
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    if (std::abs(clamped - m_fraction) < 1e-6)
        return;
    m_fraction = clamped;
    update();
}

void SegmentedBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Colour communicates urgency: cyan while comfortable, amber past 80%,
    // red once the quota is essentially spent.
    QColor fill = theme::kPrimaryButton();
    if (m_fraction >= 0.995)
        fill = theme::kRed();
    else if (m_fraction >= 0.8)
        fill = theme::kAmber();

    const qreal gap = 2.0;
    const qreal totalW = width();
    const qreal segW = (totalW - gap * (m_segments - 1)) / double(m_segments);
    if (segW <= 0.5)
        return;

    const qreal h = height();
    const double filledSegments = m_fraction * m_segments;

    p.setPen(Qt::NoPen);
    for (int i = 0; i < m_segments; ++i) {
        const QRectF r(i * (segW + gap), 0, segW, h);

        // Fully or partially filled segments get the accent colour; the rest
        // stay as a dark well.
        if (i < int(std::floor(filledSegments))) {
            p.setBrush(fill);
            p.drawRoundedRect(r, 2, 2);
        } else if (i == int(std::floor(filledSegments)) && filledSegments > 0) {
            // Partially filled leading edge: draw the well, then the slice.
            p.setBrush(theme::kPanelSunken());
            p.drawRoundedRect(r, 2, 2);

            const double frac = filledSegments - std::floor(filledSegments);
            if (frac > 0.02) {
                QRectF partial = r;
                partial.setWidth(r.width() * frac);
                p.setBrush(fill);
                p.drawRoundedRect(partial, 2, 2);
            }
        } else {
            p.setBrush(theme::kPanelSunken());
            p.drawRoundedRect(r, 2, 2);
        }
    }
}

} // namespace net
