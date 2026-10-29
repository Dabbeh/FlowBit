#pragma once

#include "appmonitor.h"

#include <QFrame>
#include <QElapsedTimer>
#include <QWidget>

class QShowEvent;
class QResizeEvent;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;
class QStackedWidget;

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

    // Animate the clipping viewport while its content keeps its natural height.
    Q_PROPERTY(int animHeight READ animHeight WRITE setAnimHeight)

public:
    explicit AppActivityPanel(AppMonitor *monitor, QWidget *parent = nullptr);
    ~AppActivityPanel() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    int animHeight() const { return m_animHeight; }
    void setAnimHeight(int height);

    bool isExpanded() const { return m_expanded; }
    bool isAnimating() const { return m_animating; }
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
    // turn it off for immediate visibility assertions.
    void setAnimated(bool animated) { m_animated = animated; }
    bool isAnimated() const { return m_animated; }

signals:
    void expandedChanged(bool expanded);
    void revealStarted(int fromHeight, int toHeight);

    // Current clip height, followed by notification when a transition settles.
    void revealTick(int height);

    void revealFinished();

private:
    int contentHeight() const;
    void layoutContent();
    void animateToHeight(int target);
    void advanceReveal();
    void finishReveal();
    void beginPreciseTiming();
    void endPreciseTiming();

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

    void rebuildTable();

    AppMonitor *m_monitor = nullptr;
    QWidget *m_content = nullptr;
    QStackedWidget *m_states = nullptr;
    int m_contentHeight = 240;
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

    QTimer *m_revealTimer = nullptr;
    QElapsedTimer m_revealClock;
    qreal m_revealTimeMs = 0;
    int m_revealDurationMs = 360;
    int m_revealFrom = 0;
    int m_revealTo = 0;
    int m_revealDirection = 1;

    // Keep a zero-height layout slot when collapsed so spacing never snaps.
    int m_animHeight = 0;

    // The same live children paint throughout; only their clipping frame moves.
    // Defer data/layout changes until the transition settles.
    bool m_animating = false;
    bool m_preciseTiming = false;
    bool m_animated = true;

    static constexpr int kTopCount = 5;
    static constexpr int kRankIntervalMs = 3000;
};

} // namespace net
