#include "toggleswitch.h"

#include "theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>

#include <QtGlobal>

namespace net {

ToggleSwitch::ToggleSwitch(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setToolTip(QStringLiteral("Enable or disable system-wide speed limiting"));
}

void ToggleSwitch::setChecked(bool checked, bool animate)
{
    if (m_checked == checked)
        return;
    m_checked = checked;

    if (animate) {
        // Animate the knob so the state change is unmistakable. knobPos is a
        // declared Q_PROPERTY, without which QPropertyAnimation would silently
        // target nothing and the knob would never move.
        auto *anim = new QPropertyAnimation(this, "knobPos", this);
        anim->setDuration(170);   // within the 150-200ms spec
        anim->setStartValue(m_knobPos);
        anim->setEndValue(checked ? 1.0 : 0.0);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
        setKnobPos(checked ? 1.0 : 0.0);
    }

    emit toggled(m_checked);
}

void ToggleSwitch::setKnobPos(qreal pos)
{
    const qreal clamped = qBound(0.0, pos, 1.0);
    if (qFuzzyCompare(clamped + 1.0, m_knobPos + 1.0))
        return;
    m_knobPos = clamped;
    update();
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const qreal h = height();
    const qreal w = width();
    const qreal radius = h / 2.0;

    // -- track ---------------------------------------------------------------
    // Monochrome, per the design system: ON is white on the dark theme and black
    // on the light one; OFF is a mid grey. The track value is the strongest state
    // signal, so it comes from the palette rather than a literal.
    QColor track = m_checked ? theme::kToggleOnTrack() : theme::kToggleOffTrack();
    if (!isEnabled())
        track = theme::kTextMuted();

    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(QRectF(0, 0, w, h), radius, radius);

    // -- knob ----------------------------------------------------------------
    // No drop shadow: the knob is a flat disc, separated from the track by value
    // and by a thin ring in the surface colour.
    const qreal knobR = radius - 3.0;
    const qreal x0 = radius;          // left resting position
    const qreal x1 = w - radius;      // right resting position
    const qreal cx = x0 + (x1 - x0) * m_knobPos;

    const QColor knob = m_checked ? theme::kToggleOnText() : theme::kTextPrimary();
    p.setPen(QPen(track, 1));
    p.setBrush(knob);
    p.drawEllipse(QPointF(cx, h / 2.0), knobR, knobR);

    // -- caption -------------------------------------------------------------
    // Exactly one plain ASCII word, on the half the knob is NOT occupying.
    //
    // The rect is inset from the knob's travel so the text can never slide under
    // the knob: an earlier version centred the label in the remaining width and
    // let it overlap, which clipped "ON" down to a stray sliver that read as a
    // corrupt "t".
    QFont f = theme::uiFont(7, true);
    f.setStyleStrategy(QFont::PreferAntialias);
    p.setFont(f);

    const QRectF captionArea(m_checked ? QRectF(0, 0, w - h, h)      // left half when ON
                                       : QRectF(h, 0, w - h, h));    // right half when OFF

    if (m_checked) {
        p.setPen(theme::kToggleOnText());     // contrasts with the ON track
        p.drawText(captionArea, Qt::AlignCenter, QStringLiteral("ON"));
    } else {
        p.setPen(theme::kToggleOffText());
        p.drawText(captionArea, Qt::AlignCenter, QStringLiteral("OFF"));
    }
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        setChecked(!m_checked);
}

void ToggleSwitch::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return) {
        setChecked(!m_checked);
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace net
