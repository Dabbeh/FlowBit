#include "trackslider.h"

#include "theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>

namespace net {

namespace {
constexpr int kTrackHeight = 6;
constexpr int kKnobRadius = 8;
constexpr int kMargin = kKnobRadius + 1;
} // namespace

TrackSlider::TrackSlider(const QColor &accent, QWidget *parent)
    : QWidget(parent), m_accent(accent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void TrackSlider::setRange(int minimum, int maximum)
{
    m_min = minimum;
    m_max = std::max(minimum + 1, maximum);
    setValue(m_value);
}

void TrackSlider::setValue(int value)
{
    const int clamped = std::clamp(value, m_min, m_max);
    const bool changed = clamped != m_value;
    m_value = clamped;
    if (changed)
        emit valueChanged(m_value);
    update();
}

void TrackSlider::setUnlimited(bool unlimited)
{
    if (m_unlimited == unlimited)
        return;
    m_unlimited = unlimited;
    update();
}

QRectF TrackSlider::trackRect() const
{
    return QRectF(kMargin, (height() - kTrackHeight) / 2.0,
                  std::max(1, width() - 2 * kMargin), kTrackHeight);
}

int TrackSlider::valueToPos(int value) const
{
    const QRectF t = trackRect();
    const double frac = double(value - m_min) / double(m_max - m_min);
    return int(t.left() + frac * t.width());
}

int TrackSlider::posToValue(int x) const
{
    const QRectF t = trackRect();
    const double frac = std::clamp((x - t.left()) / std::max(1.0, t.width()), 0.0, 1.0);
    return m_min + int(frac * (m_max - m_min) + 0.5);
}

void TrackSlider::setAccent(const QColor &accent)
{
    if (m_accent == accent)
        return;
    m_accent = accent;
    update();
}

void TrackSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track = trackRect();

    // Inactive track: dark grey on the dark theme, light grey on the light one.
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kBorderStrong());
    p.drawRoundedRect(track, kTrackHeight / 2.0, kTrackHeight / 2.0);

    if (m_unlimited) {
        // Nothing to fill; draw a muted knob parked at the left.
        p.setPen(Qt::NoPen);
        p.setBrush(theme::kTextMuted());
        p.drawEllipse(QPointF(track.left(), track.center().y()), kKnobRadius - 3,
                      kKnobRadius - 3);
        return;
    }

    const int knobX = valueToPos(m_value);

    // Active track: a single flat monochrome fill. The previous version used a
    // gradient plus an alpha "halo", which is exactly the glow this design
    // removes - emphasis now comes from the value contrast alone.
    const QRectF filled(track.left(), track.top(), knobX - track.left(), track.height());
    if (filled.width() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(theme::kPrimaryButton());
        p.drawRoundedRect(filled, kTrackHeight / 2.0, kTrackHeight / 2.0);
    }

    // Knob: solid, with a ring in the surface colour so it stays readable where
    // it overlaps the filled and unfilled halves.
    const QPointF knob(knobX, track.center().y());
    p.setPen(QPen(theme::kPanel(), 2));
    p.setBrush(theme::kPrimaryButton());
    p.drawEllipse(knob, kKnobRadius - 2, kKnobRadius - 2);

    // A slightly larger ring while dragging: the only motion cue, no glow.
    if (m_dragging) {
        p.setPen(QPen(theme::kPrimaryButton(), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(knob, kKnobRadius + 1, kKnobRadius + 1);
    }
}

void TrackSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;

    m_dragging = true;
    // Any interaction implies the user wants a finite value.
    if (m_unlimited) {
        m_unlimited = false;
        emit valueChanged(m_value);
    }
    setValue(posToValue(int(event->position().x())));
    update();
}

void TrackSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    setValue(posToValue(int(event->position().x())));
}

void TrackSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    update();
    emit sliderReleased();
}

void TrackSlider::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0)
        return;
    if (m_unlimited && steps > 0) {
        m_unlimited = false;
        emit valueChanged(m_value);
    }
    setValue(m_value + steps * std::max(1, (m_max - m_min) / 100));
    emit sliderReleased();
    event->accept();
}

} // namespace net
