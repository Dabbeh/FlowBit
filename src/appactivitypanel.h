#pragma once

#include "appmonitor.h"

#include <QFrame>
#include <QWidget>

class QShowEvent;
class QResizeEvent;

class QPropertyAnimation;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

namespace net {

class BusyDots;

// The expandable "Application Network Activity" panel.
//
// Collapsed by default: the dashboard's Live Speed card gets a "Show More"
// button, and this panel is the section that appears beneath the top row when it
// is pressed. It is a separate full-width card rather than an expansion of Live
// Speed, because Live Speed sits beside the usage card and growing only one of
// them would make the row lopsided.
//
// The panel never invents data. Three states are distinct and all reachable:
//
//   * Monitoring unavailable  - the backend could not start; the reason is shown
//     with a Retry button. A failed backend must never render as a list of zeroes.
//   * No active applications  - the backend is running and nothing is transferring.
//   * A populated list        - real measured rates.
class AppActivityPanel : public QFrame
{
    Q_OBJECT

    // The height the panel is currently animating to.
    //
    // Exposed as a property so QPropertyAnimation can drive it, but implemented to
    // set minimum AND maximum together: that pins the panel's box and stops the
    // parent layout from re-flowing every card on every frame, which is what made
    // the reveal feel like the page was being pushed. -1 means "no constraint".
    Q_PROPERTY(int animHeight READ animHeight WRITE setAnimHeight)

public:
    explicit AppActivityPanel(AppMonitor *monitor, QWidget *parent = nullptr);

    int animHeight() const { return m_animHeight; }
    void setAnimHeight(int height);

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);

    // Drives the panel from the monitor. Called once per monitor interval while
    // expanded, and not at all while collapsed.
    void refresh();

    // Column currently used for sorting, and its direction.
    enum class SortBy { Bandwidth, Name, Download, Upload };

    // Applies a sort as if the given column header had been clicked. Public
    // because the header wiring and the tests both go through it.
    void sortByColumn(int column);
    SortBy sortColumn() const { return m_sortBy; }
    bool sortDescending() const { return m_sortDescending; }

    // Expand/collapse tween. On by default for the app; the offscreen suites
    // turn it off so visibility assertions never have to race a 160ms tween.
    void setAnimated(bool animated) { m_animated = animated; }
    bool isAnimated() const { return m_animated; }

signals:
    void expandedChanged(bool expanded);

    // Emitted once the reveal or collapse tween has run to completion - NOT when
    // it is interrupted by a new toggle. The dashboard uses this to decide
    // whether (and by how little) to glide the viewport, because only a finished
    // tween leaves the scroll range in its final state.
    // Emitted on every animated height change while the reveal or collapse runs.
    // The dashboard follows the growing bottom edge with the viewport in the SAME
    // frames as the growth, which reads as one continuous motion; the previous
    // unroll-then-glide sequence made the top row dip and spring back.
    void revealTick(int height);

    void revealFinished();

private:
    // Rebuilds the opacity effect after the widget has been re-parented,
    // which destroys the previous one. Also re-points the fade animation,
    // which would otherwise hold a dead target.

    // Sizes the three numeric columns to the widest string they can display and
    // lets the name column take the rest. Re-run on font or theme changes.
    void applyNumericColumnWidths();

    // Recomputes the column widths for the new width.
    //
    // The columns share the available space, so a resize changes how much each gets. Without
    // this a window narrowed and then widened would keep the narrow widths it was given - a
    // worse fault than the one being fixed, because the columns would stay squeezed with room
    // to spare.
    void resizeEvent(QResizeEvent *event) override;

    // Re-applies the column widths when the panel becomes visible.
    //
    // The widths come from the table's own width, and a hidden table has none - so the single
    // call made while building the panel measured a collapsed layout and set the floor widths.
    // resizeEvent cannot cover this, because expanding the panel does not necessarily resize it.
    void showEvent(QShowEvent *event) override;

    // Recomputes the column widths, guarded against re-entering from the layout pass they cause.
    void refreshColumnWidths();

    // Guards applyNumericColumnWidths against re-entering from the layout pass it causes.
    bool m_adjustingColumns = false;

    // Applies the visibility rules for the current state (error / empty / rows) so
    // that a following sizeHint() reports the height of what will actually be shown.
    // A hidden child still counts toward its layout's size hint, which is why the
    // reveal target used to overshoot by the whole table.
    void updateStateVisibility();

    void rebuildTable();
    void updateEmptyStates();
    void onHeaderClicked(int column);

    AppMonitor *m_monitor = nullptr;
    bool m_expanded = false;
    bool m_showAll = false;
    bool m_hasData = false;

    // Ranking is refreshed on its own slower cadence: rates fluctuate every
    // second, and reordering rows at that rate makes the list unreadable.
    QTimer *m_rankTimer = nullptr;
    QVector<AppTraffic> m_ranked;

    SortBy m_sortBy = SortBy::Bandwidth;
    bool m_sortDescending = true;

    // Anti-flicker persistence: an application that drops to zero keeps its row
    // for a few refreshes (shown at 0 B/s) before it is removed, so bursty
    // traffic does not make rows blink in and out every second.
    QHash<QString, QPair<int, AppTraffic>> m_persist;

    QTableWidget *m_table = nullptr;
    QWidget *m_emptyState = nullptr;
    QWidget *m_errorState = nullptr;
    QLabel *m_errorText = nullptr;
    QLabel *m_emptyTitle = nullptr;   // swaps between "Starting…" and the empty state
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_viewAllButton = nullptr;
    QPushButton *m_showLessButton = nullptr;
    QLabel *m_unattributedLabel = nullptr;
    QLabel *m_scopeNoteLabel = nullptr;
    BusyDots *m_busyDots = nullptr;   // animated wait indicator

    // Set when a collapse wants the trace session released, but the
    // collapse animation is still running. The stop then happens once the
    // animation finishes: stopping mid-animation blocked the UI thread
    // against the very widgets being animated, which crashed the app.
    bool m_stopMonitorWhenCollapsed = false;   // shown when measuring TCP only

    // Expand/collapse tween state.
    // The reveal/collapse tween. A single property animation on animHeight:
    // the parallel height+opacity group that preceded it invalidated the whole
    // dashboard layout every frame and forced an offscreen render, which is
    // what made the motion feel like a shove and stutter.
    QPropertyAnimation *m_expandAnim = nullptr;
    // The reveal-only opacity effect. Lives exactly as long as the tween:
    // destroyed when the fade completes so no offscreen render cost survives it.

    // Current tween height, -1 when unconstrained.
    int m_animHeight = -1;

    // True while a reveal or collapse is in flight; the table rebuild is
    // skipped during this so frames are not spent rebuilding rows.
    bool m_animating = false;
    int m_maxHeight = QWIDGETSIZE_MAX;
    bool m_animated = true;

    static constexpr int kTopCount = 5;
    static constexpr int kRankIntervalMs = 3000;
};

} // namespace net
