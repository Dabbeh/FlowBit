#include "scrollbaranimator.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QTimer>

namespace net {

namespace {

// Marks a scroll area as already handled, so install() over a whole page cannot add the same
// filter twice. Two filters would each animate towards the same value and fight.
const char *kInstalledProperty = "flowbitScrollAnimated";

} // namespace

ScrollBarAnimator::ScrollBarAnimator(QAbstractScrollArea *area)
    : QObject(area), m_area(area), m_bar(area ? area->verticalScrollBar() : nullptr)
{
    if (m_area)
        m_area->installEventFilter(this);
    // The bar itself is filtered too: the handle sits inside it, and the pointer entering the
    // bar directly - rather than the viewport - is a hover the area alone would miss.
    if (m_bar)
        m_bar->installEventFilter(this);
}

void ScrollBarAnimator::install(QWidget *root)
{
    if (!root)
        return;

    QList<QAbstractScrollArea *> areas;
    if (auto *self = qobject_cast<QAbstractScrollArea *>(root))
        areas << self;
    areas << root->findChildren<QAbstractScrollArea *>();

    int added = 0;
    for (QAbstractScrollArea *area : areas) {
        if (!area || area->property(kInstalledProperty).toBool())
            continue;
        area->setProperty(kInstalledProperty, true);
        new ScrollBarAnimator(area);
        ++added;
    }
}

void ScrollBarAnimator::setFadeEnabled(bool enabled)
{
    if (m_fadeEnabled == enabled)
        return;
    m_fadeEnabled = enabled;

    if (!m_fadeEnabled) {
        // Show it and stop. A scrollbar that never appears is worse than one that never fades,
        // which is why this is a choice rather than the always-on behaviour.
        settle();
        if (m_bar)
            m_bar->setGraphicsEffect(nullptr);
    } else if (m_hovered) {
        fadeTo(1.0);
    }
}

void ScrollBarAnimator::fadeTo(qreal to)
{
    if (!m_bar)
        return;

    // The effect is created on demand. Attaching one permanently would cost an offscreen render
    // on every repaint of a widget that is most often not being looked at.
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(m_bar->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(m_bar);
        effect->setOpacity(to);
        m_bar->setGraphicsEffect(effect);
    }

    if (!m_anim) {
        m_anim = new QPropertyAnimation(effect, "opacity", this);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        m_anim->setDuration(kFadeMs);
    }

    m_anim->stop();
    m_anim->setStartValue(effect->opacity());
    m_anim->setEndValue(to);
    m_anim->start();
}

void ScrollBarAnimator::settle()
{
    if (m_anim)
        m_anim->stop();
    // The effect is removed rather than left at zero opacity: an effect on a widget costs an
    // offscreen buffer per paint even when the result is invisible.
    if (m_bar)
        m_bar->setGraphicsEffect(nullptr);
}

bool ScrollBarAnimator::eventFilter(QObject *watched, QEvent *event)
{
    // A scroll area can be deleted while its animator is alive, hence the guarded pointers.
    if (!m_area || !m_bar)
        return QObject::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::Enter:
        m_hovered = true;
        if (m_fadeEnabled)
            fadeTo(1.0);
        break;

    case QEvent::Leave:
        m_hovered = false;
        // Not while it is being dragged: the handle is in use, and fading it out under the
        // pointer that is holding it would be wrong even though the pointer has left the area.
        if (m_fadeEnabled && !m_bar->isSliderDown())
            fadeTo(0.0);
        break;

    case QEvent::Hide:
        // A page that is no longer shown should not keep a fade running.
        settle();
        break;

    default:
        break;
    }

    // Never consumed: this only observes, and swallowing events here would break scrolling.
    return QObject::eventFilter(watched, event);
}

} // namespace net
