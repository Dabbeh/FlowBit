#pragma once

#include <QWidget>

class QBoxLayout;

namespace net {

// Lays two widgets side by side, or stacked when the container is too narrow for
// both.
//
// This exists because a plain QHBoxLayout with stretch factors does not degrade
// gracefully: when the combined minimum width exceeds what is available, Qt keeps
// every child at its minimum and the PROPORTIONS invert - at the 880px window
// minimum the Live Speed card ended up narrower than the Usage card despite
// asking for a 62/38 split. Stacking keeps each widget at full width instead of
// distorting the layout.
//
// The children are handed over with setPair() AFTER construction, and are
// expected to have been created with this widget as their parent. Reparenting
// them afterwards would detach each card frame from its own contents.
class ResponsivePair : public QWidget
{
    Q_OBJECT

public:
    explicit ResponsivePair(QWidget *parent = nullptr);

    // Adopts the two widgets. Both must already be parented to this pair.
    void setPair(QWidget *first, QWidget *second, int firstStretch, int secondStretch);

    // Places `w` between the two children in the stacked orientation. Only valid
    // while stacked; takeMiddle() hands it back for the wide layout.
    void embedMiddle(QWidget *w);

    // Removes the middle widget from this pair and detaches it (hidden). The
    // caller re-parents it into its wide-layout home.
    void takeMiddle(QWidget *w);

    bool isStacked() const { return m_stacked; }

    // Combined width below which the two are stacked vertically.
    void setStackThreshold(int px);

signals:
    // Emitted when the orientation flips, so the owner can move a middle widget
    // in or out of the pair.
    void orientationChanged(bool stacked);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyLayout();

    QWidget *m_first = nullptr;
    QWidget *m_second = nullptr;
    QWidget *m_middle = nullptr;
    QBoxLayout *m_layout = nullptr;
    int m_firstStretch = 1;
    int m_secondStretch = 1;
    int m_threshold = 900;
    // Shared card height, measured after polishing and held during middle-panel
    // transitions so the layout cannot resize the graph to redistribute space.
    int m_childHeight = 0;

    bool m_stacked = false;
    bool m_arranged = false;
};

} // namespace net
