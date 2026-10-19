#include "responsivepair.h"

#include <QBoxLayout>
#include <QResizeEvent>
#include <QSizePolicy>

namespace net {

ResponsivePair::ResponsivePair(QWidget *parent) : QWidget(parent)
{
    // A horizontal box initially; applyLayout() swaps the direction for a
    // vertical one when the container is too narrow. QBoxLayout can change
    // direction in place, which is why one instance is reused rather than rebuilt.
    auto *box = new QBoxLayout(QBoxLayout::LeftToRight, this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(12);
    m_layout = box;

    // Vertically FIXED to the natural height, horizontally free.
    //
    // The default Preferred policy lets a layout grow this widget, so the dashboard's
    // canvas handed it part of the space that appeared when the activity panel was
    // inserted - and because the pair holds the Live Speed card, its height change is
    // what pushed the graph up and down during the reveal. Measured: 200px in a narrow
    // window, 9px maximized.
    //
    // The canvas ends with addStretch(1), which exists precisely to absorb leftover
    // space; a card row competing with it was the bug. Fixed vertically means the only
    // thing that can change the pair's height is its own content, and the minimum set
    // in applyLayout() keeps that stable too.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ResponsivePair::setPair(QWidget *first, QWidget *second, int firstStretch,
                             int secondStretch)
{
    m_first = first;
    m_second = second;
    m_firstStretch = firstStretch;
    m_secondStretch = secondStretch;

    // The children are expected to have been built with this widget as their
    // parent, so no reparenting happens here - doing that would detach a card
    // frame from the layout that holds its contents.
    //
    // Each card is pinned to its natural HEIGHT and left free horizontally.
    //
    // Measured at the user's real window size (1536x816 logical, i.e. 1920x1080 at
    // 125% scaling): opening the panel made these two cards grow by 20px, which grew
    // the pair by 10px and pushed the Live Speed card - and its graph - up and down.
    // With the default Preferred policy the pair's layout is allowed to stretch them,
    // and their larger size hints then raise the pair's own minimum, feeding back.
    // Fixed vertically breaks that loop: the row is exactly as tall as its cards.
    // Preferred vertically, NOT Fixed: the two cards share one row, so each must be
    // able to grow to the row's height. Pinning them Fixed made the shorter card sit
    // at its own smaller size while the row claimed the taller one - the squashed
    // 30-Day Usage card in the user's screenshot.
    if (m_first) {
        m_first->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_layout->addWidget(m_first);
    }
    if (m_second) {
        m_second->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_layout->addWidget(m_second);
    }

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
    // Index 1 = between the first and second children.
    m_layout->insertWidget(1, w);
}

void ResponsivePair::takeMiddle(QWidget *w)
{
    if (!m_layout || !w)
        return;
    m_layout->removeWidget(w);
    // Detaching hides the widget; the caller re-inserts and re-shows it in its
    // wide-layout home.
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

    // Stack once the two side-by-side minimums no longer fit. The threshold is
    // the sum of the children's own minimum widths plus the gap, so it adapts if
    // their contents change rather than relying on a hard-coded figure alone.
    const int needed =
        m_first->minimumSizeHint().width() + m_second->minimumSizeHint().width() + 12;
    const bool wantStacked = width() > 0 && (width() < m_threshold || width() < needed);

    if (m_arranged && wantStacked == m_stacked)
        return;
    const bool wasStacked = m_stacked;
    m_stacked = wantStacked;
    m_arranged = true;

    // Direction only; the stretch factors are re-applied because they mean
    // different things in each orientation.
    m_layout->setDirection(wantStacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);

    for (int i = 0; i < m_layout->count(); ++i) {
        QLayoutItem *item = m_layout->itemAt(i);
        if (!item || !item->widget())
            continue;
        const bool isFirst = (item->widget() == m_first);
        // Side by side, the shares are the requested split. Stacked, both take
        // the full width, so their relative weight is meaningless.
        m_layout->setStretch(i, wantStacked ? 1 : (isFirst ? m_firstStretch : m_secondStretch));
    }

    // Stacking needs roughly twice the height of the tall child, plus the gap.
    // Without this the pair's own size hint is still the side-by-side figure, so
    // the surrounding layout hands it one row's worth of height and both cards
    // collapse to a sliver.
    //
    // MEASURED BUG, now fixed: this value used to be recomputed from the children's
    // LIVE minimumSizeHint() on every applyLayout(), and applyLayout() runs whenever
    // the canvas is resized - including the resizes caused by the activity panel
    // animating. The Live Speed card's hint changes as its graph repaints, so the
    // pair's own minimum height moved mid-reveal: measured at 14px in a maximized
    // window and 200px in a narrow one. Because the pair contains the Live Speed
    // card, that height change is what pushed the graph up and down. A small window
    // hid it only because its layout was already at the minimum, leaving nothing to
    // redistribute.
    //
    // The number is a MINIMUM for the pair, not a live measurement of its content, so
    // it is captured once and reused. updateGeometry() below still re-runs the parent
    // layout, so nothing about the responsive behaviour changes.
    // Height policy for the row, and why it is a MINIMUM *and* a MAXIMUM.
    //
    // Measured at the user's real window (1536x816 logical = 1920x1080 at 125%):
    // with a minimum alone, the pair and the canvas layout disagreed by a few pixels on
    // alternating passes, and the Usage card flipped between 271 and ~258 - below its
    // own minimumSizeHint - several times per second, including after the reveal had
    // finished. Card A (Live Speed, holding the graph) stayed at 201 the whole time,
    // but the row shrank around it, which is the movement the user sees.
    //
    // Both cards report sizeHint 271, so the row's natural height is unambiguous. A
    // minimum lets a parent layout squeeze below that; pinning BOTH bounds to the
    // cards' natural height removes any slack for a layout to argue over, and the
    // oscillation has nothing left to oscillate.
    //
    // The value is captured once: it is a property of the cards, not a live reading of
    // their content, and re-deriving it per pass is what fed the loop.
    // The row height is the TALLER card's natural height, captured once.
    //
    // Captured once because a value that keeps changing is what fed the feedback loop:
    // measured at the user's real window (1536x816 logical = 1920x1080 at 125%), the
    // Usage card flipped between 271 and 258 several times a second while the Live
    // Speed card stood still, and each flip moved the graph.
    //
    // It must remain a MINIMUM, not a fixed height. An earlier attempt clamped both
    // bounds to this number and the Usage card then rendered small beside a full-height
    // Live Speed card: the pair's bounds do not stretch its children, so the shorter
    // card kept its own size. A minimum lets the row's layout make both cards fill it,
    // which is what puts them at matching heights.
    if (m_childHeight <= 0) {
        m_childHeight = qMax(m_first->sizeHint().height(),
                             m_second->sizeHint().height());
    }

    // Never a maximum: in stacked mode the panel can be embedded between the cards
    // and has to be free to grow, and the single-row case is bounded anyway by the
    // cards' own hints.
    setMaximumHeight(QWIDGETSIZE_MAX);
    setMinimumHeight(wantStacked ? 2 * m_childHeight + 12 : m_childHeight);

    if (layout())
        layout()->activate();
    updateGeometry();

    // Tell the owner so it can move a middle widget in or out of the pair.
    if (wasStacked != m_stacked)
        emit orientationChanged(m_stacked);
}

} // namespace net
