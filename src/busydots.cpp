#include "busydots.h"

#include "theme.h"

#include <QFontMetrics>
#include <QPainter>
#include <QTimer>

namespace net {

namespace {
constexpr int kDotCount = 3;
constexpr int kDotRadius = 2;
constexpr int kDotGap = 7;
constexpr int kTickMs = 260;   // calm, not a frantic spinner
constexpr int kLabelGap = 10;
} // namespace

BusyDots::BusyDots(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &BusyDots::tick);
}

void BusyDots::setLabel(const QString &label)
{
    if (m_label == label)
        return;
    m_label = label;
    updateGeometry();
    update();
}

QSize BusyDots::sizeHint() const
{
    const QFontMetrics fm(theme::uiFont(9, false));
    const int dots = kDotCount * kDotGap;
    const int text = m_label.isEmpty() ? 0 : (kLabelGap + fm.horizontalAdvance(m_label));
    return QSize(dots + text + 4, qMax(16, fm.height()));
}

void BusyDots::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int cy = height() / 2;
    for (int i = 0; i < kDotCount; ++i) {
        // The active dot is fully opaque and its neighbours fade out behind it, so
        // the motion reads as a travelling highlight rather than a blink - the same
        // idea as the speed graph, which is the app's other moving element.
        const int distance = (i - m_phase + kDotCount) % kDotCount;
        const int alpha = (distance == 0) ? 255 : (distance == 1 ? 140 : 60);

        QColor c = theme::kTextSecondary();
        c.setAlpha(alpha);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPoint(kDotRadius + 2 + i * kDotGap, cy), kDotRadius, kDotRadius);
    }

    if (!m_label.isEmpty()) {
        p.setFont(theme::uiFont(9, false));
        p.setPen(theme::kTextMuted());
        const int x = kDotRadius * 2 + 2 + kDotCount * kDotGap + kLabelGap;
        p.drawText(QRect(x, 0, width() - x, height()),
                   Qt::AlignLeft | Qt::AlignVCenter, m_label);
    }
}

void BusyDots::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_timer->start();
}

void BusyDots::hideEvent(QHideEvent *event)
{
    // Stop when invisible: a waiting indicator that is not on screen must not
    // schedule repaints.
    m_timer->stop();
    QWidget::hideEvent(event);
}

void BusyDots::tick()
{
    m_phase = (m_phase + 1) % kDotCount;
    update();
}

} // namespace net
