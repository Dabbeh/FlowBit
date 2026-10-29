#include "responsivepair.h"

#include <QBoxLayout>
#include <QResizeEvent>
#include <QSizePolicy>

namespace net {

ResponsivePair::ResponsivePair(QWidget *parent) : QWidget(parent)
{
    auto *box = new QBoxLayout(QBoxLayout::LeftToRight, this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(12);
    m_layout = box;

    // Let the dashboard's trailing stretch absorb spare vertical space.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ResponsivePair::setPair(QWidget *first, QWidget *second, int firstStretch,
                             int secondStretch)
{
    m_first = first;
    m_second = second;
    m_firstStretch = firstStretch;
    m_secondStretch = secondStretch;
    if (!m_first || !m_second)
        return;

    m_layout->addWidget(m_first);
    m_layout->addWidget(m_second);

    // Measure after stylesheet polishing and give both cards the same height.
    // An embedded reveal can then grow without redistributing space between
    // the Live Speed and Usage cards on every animation frame.
    m_first->ensurePolished();
    m_second->ensurePolished();
    m_childHeight = qMax(qMax(m_first->sizeHint().height(), m_second->sizeHint().height()),
                         qMax(m_first->minimumHeight(), m_second->minimumHeight()));
    m_first->setFixedHeight(m_childHeight);
    m_second->setFixedHeight(m_childHeight);
    applyLayout();
}

void ResponsivePair::setStackThreshold(int px)
{
    m_threshold = px;
    applyLayout();
}

void ResponsivePair::embedMiddle(QWidget *w)
{
    if (!m_layout || !w)
        return;
    if (w->parent() != this)
        w->setParent(this);
    m_layout->insertWidget(1, w);
}

void ResponsivePair::takeMiddle(QWidget *w)
{
    if (!m_layout || !w)
        return;
    m_layout->removeWidget(w);
    w->setParent(nullptr);
}

void ResponsivePair::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyLayout();
}

void ResponsivePair::applyLayout()
{
    if (!m_layout || !m_first || !m_second)
        return;

    const int needed =
        m_first->minimumSizeHint().width() + m_second->minimumSizeHint().width() + 12;
    const bool wantStacked = width() > 0 && (width() < m_threshold || width() < needed);
    if (m_arranged && wantStacked == m_stacked)
        return;
    const bool wasStacked = m_stacked;
    m_stacked = wantStacked;
    m_arranged = true;

    m_layout->setDirection(wantStacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    for (int i = 0; i < m_layout->count(); ++i) {
        QLayoutItem *item = m_layout->itemAt(i);
        if (!item || !item->widget())
            continue;
        const bool isFirst = item->widget() == m_first;
        m_layout->setStretch(i, wantStacked ? 0 : (isFirst ? m_firstStretch : m_secondStretch));
    }

    // The pair can grow to include an animated middle panel. Its two cards
    // retain their shared height in both orientations.
    setMaximumHeight(QWIDGETSIZE_MAX);
    setMinimumHeight(wantStacked ? 2 * m_childHeight + 12 : m_childHeight);
    m_layout->activate();
    updateGeometry();

    if (wasStacked != m_stacked)
        emit orientationChanged(m_stacked);
}

} // namespace net
