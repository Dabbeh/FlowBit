#include "appactivitypanel.h"

#include "appnames.h"
#include "busydots.h"
#include "theme.h"

#include <QFrame>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QResizeEvent>
#include <QShowEvent>
#include <QScrollBar>
#include <QLabel>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QDebug>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>

namespace net {

namespace {

// The narrowest a numeric column may be squeezed. Below this a rate is unreadable even elided,
// so it is a floor for legibility rather than for the arithmetic.
constexpr int kMinColumnWidth = 56;

// Rates are rendered with the app's existing formatting rules, so the units here
// match the Live Speed readout exactly. Bytes, not bits - 1 MB/s is 8 Mbps, but
// the label always says MB/s.
QString rateText(double bytesPerSec)
{
    return theme::formatBytesPerSec(bytesPerSec);
}

QTableWidgetItem *makeItem(const QString &text, Qt::Alignment align)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(align | Qt::AlignVCenter);
    item->setFlags(Qt::ItemIsEnabled);   // read-only
    return item;
}

} // namespace

AppActivityPanel::AppActivityPanel(AppMonitor *monitor, QWidget *parent)
    : QFrame(parent), m_monitor(monitor)
{
    setObjectName(QStringLiteral("card"));
    setStyleSheet(theme::cardStyleSheet(QStringLiteral("card")));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    // -- header ------------------------------------------------------------
    auto *header = new QHBoxLayout();
    header->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("APPLICATION NETWORK ACTIVITY"), this);
    title->setObjectName(QStringLiteral("cardTitle"));
    header->addWidget(title);
    header->addStretch(1);

    root->addLayout(header);

    // -- table -------------------------------------------------------------
    // Column headings take the graph's series colours on the arrows only: the
    // cyan/orange pairing is the app's existing download/upload language, and
    // headings are where a reader looks to learn it.
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("Application"), QStringLiteral("Download"),
         QStringLiteral("Upload"), QStringLiteral("Total")});

    const auto colourHeading = [&](int column, const QColor &accent) {
        if (QTableWidgetItem *h = m_table->horizontalHeaderItem(column))
            h->setForeground(accent);
    };
    colourHeading(1, theme::kGraphDownload());
    colourHeading(2, theme::kGraphUpload());
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setAlternatingRowColors(false);
    m_table->horizontalHeader()->setSectionsClickable(true);
    m_table->horizontalHeader()->setHighlightSections(false);
    // Column sizing, and why it is done by hand.
    //
    // ResizeToContents on every numeric column let their combined demand exceed the
    // panel width; QHeaderView then pushed the final section past the right edge, so
    // "Upload" was clipped and "Total" was invisible.
    //
    // ALL FOUR ARE INTERACTIVE, including the name column.
    //
    // Stretch was used for the name column so it would take the remainder. Stretch also IGNORES
    // setColumnWidth - it recomputes its own width on every layout pass from whatever space is
    // left. The numeric widths were set and then the name column claimed everything above them,
    // which is why the measurements showed [947 56 56 56]: three columns pinned at their floor
    // while the name column took the rest.
    //
    // With all four Interactive, the widths computed below are the widths that are used. The
    // remainder is handed to the name column explicitly, and it is recalculated on every resize,
    // so nothing is lost by giving up Stretch.
    for (int c = 0; c <= 3; ++c)
        m_table->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Interactive);

    applyNumericColumnWidths();

    m_table->setIconSize(QSize(20, 20));
    m_table->setMinimumHeight(160);
    // Reserve the scrollbar inside the table instead of letting it overlay the last
    // column: without this the reserved width and the painted layout disagree and
    // "Total" is drawn under the scrollbar.
    // ALWAYS ON, not AsNeeded. With AsNeeded the scrollbar appeared for a moment
    // while the panel opened and then vanished; each transition changes the viewport
    // width by the scrollbar's width (10px in this theme), which slid the column
    // headings sideways and forced the section to re-measure. Reserving the space
    // permanently costs 10px of a 1000px+ card and removes the movement entirely.
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this,
            &AppActivityPanel::sortByColumn);

    root->addWidget(m_table, 1);

    // -- empty state -------------------------------------------------------
    m_emptyState = new QWidget(this);
    auto *emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(0, 20, 0, 20);
    emptyLayout->setSpacing(6);

    auto *emptyTitle =
        new QLabel(QStringLiteral("No active network applications"), m_emptyState);
    emptyTitle->setObjectName(QStringLiteral("statValue"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);
    m_emptyTitle = emptyTitle;   // refreshed per tick: also announces "Starting…"
    // Panel member retained so the async start can swap this text without
    // rebuilding the layout every second.

    auto *emptyBody = new QLabel(
        QStringLiteral("Applications using your internet connection will appear here."),
        m_emptyState);
    emptyBody->setObjectName(QStringLiteral("sub"));
    emptyBody->setAlignment(Qt::AlignCenter);
    emptyBody->setWordWrap(true);
    emptyLayout->addWidget(emptyBody);

    // Waiting indicator, centred under the message. Shown only while the session
    // is still starting, so a wait looks like a wait instead of a settled, empty
    // result. It stops its timer when hidden (see BusyDots::hideEvent).
    m_busyDots = new BusyDots(m_emptyState);
    emptyLayout->addWidget(m_busyDots, 0, Qt::AlignHCenter);
    m_busyDots->setVisible(false);

    root->addWidget(m_emptyState);

    // -- error state -------------------------------------------------------
    m_errorState = new QWidget(this);
    auto *errLayout = new QVBoxLayout(m_errorState);
    errLayout->setContentsMargins(0, 16, 0, 16);
    errLayout->setSpacing(8);

    auto *errTitle = new QLabel(QStringLiteral("Application monitoring unavailable"),
                                m_errorState);
    errTitle->setObjectName(QStringLiteral("statValue"));
    errTitle->setAlignment(Qt::AlignCenter);
    errLayout->addWidget(errTitle);

    m_errorText = new QLabel(m_errorState);
    m_errorText->setObjectName(QStringLiteral("sub"));
    m_errorText->setAlignment(Qt::AlignCenter);
    m_errorText->setWordWrap(true);
    errLayout->addWidget(m_errorText);

    m_retryButton = new QPushButton(QStringLiteral("Retry"), m_errorState);
    m_retryButton->setCursor(Qt::PointingHandCursor);
    connect(m_retryButton, &QPushButton::clicked, this, [this] {
        if (m_monitor)
            m_monitor->startAsync();
        refresh();
    });
    errLayout->addWidget(m_retryButton, 0, Qt::AlignHCenter);

    root->addWidget(m_errorState);

    // -- footer ------------------------------------------------------------
    auto *footer = new QHBoxLayout();
    footer->setSpacing(8);

    m_unattributedLabel = new QLabel(this);
    m_scopeNoteLabel = new QLabel(this);
    m_scopeNoteLabel->setObjectName(QStringLiteral("scopeNote"));
    m_scopeNoteLabel->setVisible(false);
    // Filled from AppMonitor::fallbackReason() whenever the reduced source is in
    // use, so the note states the ACTUAL cause rather than a fixed sentence.
    m_scopeNoteLabel->setText(QStringLiteral(
        "Measuring TCP traffic only \u2014 UDP/QUIC apps may read lower than the "
        "system total."));
    // Wrapped, because an unwrapped label demands its whole text on one line and a layout takes
    // that as a minimum width for the panel. A sentence here made the panel wider than the
    // window, which pushed the table's columns off the right edge - in a full-screen window as
    // much as a small one. The column arithmetic was never at fault; the panel it divided had
    // been stretched by a caption.
    m_scopeNoteLabel->setWordWrap(true);
    m_scopeNoteLabel->setToolTip(m_scopeNoteLabel->text());
    m_unattributedLabel->setObjectName(QStringLiteral("sub"));
    // Wrapped for the same reason as the note above: an unwrapped sentence sets a minimum on
    // the whole panel.
    m_unattributedLabel->setWordWrap(true);
    m_unattributedLabel->setToolTip(QStringLiteral(
        "Traffic belonging to system or protected processes, which cannot be "
        "attributed to a user application."));
    footer->addWidget(m_unattributedLabel);
    footer->addWidget(m_scopeNoteLabel);
    footer->addStretch(1);

    m_viewAllButton = new QPushButton(this);
    m_viewAllButton->setObjectName(QStringLiteral("ghost"));
    m_viewAllButton->setCursor(Qt::PointingHandCursor);
    connect(m_viewAllButton, &QPushButton::clicked, this, [this] {
        m_showAll = !m_showAll;
        rebuildTable();
    });
    footer->addWidget(m_viewAllButton);

    m_showLessButton = new QPushButton(QStringLiteral("Show Less \u02C4"), this);
    m_showLessButton->setObjectName(QStringLiteral("ghost"));
    m_showLessButton->setCursor(Qt::PointingHandCursor);
    connect(m_showLessButton, &QPushButton::clicked, this, [this] { setExpanded(false); });
    footer->addWidget(m_showLessButton);

    root->addLayout(footer);

    // -- expand/collapse tween ----------------------------------------------
    // Animates the panel's own height through a dedicated property.
    //
    // Why not maximumHeight: that invalidates the parent layout on every frame, so
    // the scroll area re-laid out the whole dashboard canvas ~60 times a second.
    // The visible result was the page shoving itself around instead of the panel
    // opening - the reported "it pushes" behaviour.
    //
    // animHeight() below sets both minimumHeight and maximumHeight to the same
    // value, which pins the panel's box and lets the layout below it settle once
    // when the tween ends rather than on every step.
    m_expandAnim = new QPropertyAnimation(this, "animHeight", this);
    // 280ms InOutSine, and the duration is tied to the curve rather than chosen
    // independently: InOutSine starts at ZERO velocity, so its opening frames cover
    // very little distance. At 220ms that slow start read as hesitation; 280ms gives
    // the curve room to accelerate through the middle and settle at the end, and it
    // is still inside the 200-300ms band these reveals normally use.
    //
    // Why this curve: the reveal must read as ONE motion. An ease-out curve
    // (OutQuad, OutCubic) begins at well above its average speed, so the first
    // displayed frame is a visible step - measured at ~50px of a 514px travel, which
    // is what "it pushes" was. Qt's own description of InOutSine is "acceleration
    // until halfway, then deceleration", which is the shape wanted here.
    //
    // Deliberately NOT animated alongside the height:
    //   * a content fade, which forces an offscreen render of the whole panel (table
    //     included) every frame - the reported "it lags"
    //   * a settle offset, a second simultaneous motion - the reported "wobble"
    // Every extra motion competing for the shared animation timer is one more thing
    // that can be seen as a separate movement instead of part of the reveal.
    m_expandAnim->setDuration(280);
    m_expandAnim->setEasingCurve(QEasingCurve::InOutSine);

    connect(m_expandAnim, &QVariantAnimation::finished, this, [this] {
        if (!m_expanded) {
            // Fully collapsed: hide, and release the height constraint so the
            // widget is never left pinned if it is shown again.
            setVisible(false);
            m_animHeight = -1;
            setMinimumHeight(0);
            setMaximumHeight(QWIDGETSIZE_MAX);

            // The collapse has finished, so nothing is animating any more: the
            // safe moment to tear the trace session down.
            if (m_stopMonitorWhenCollapsed && m_monitor) {
                m_stopMonitorWhenCollapsed = false;
                QTimer::singleShot(0, this, [this] {
                    if (m_monitor && !m_expanded && m_monitor->isRunning())
                        m_monitor->stop();
                });
            }
        } else {
            // Fully expanded: KEEP the height pin at the tween's end value.
            //
            // Releasing it here used to snap the panel to whatever height the
            // surrounding layout allocated - often well above the animated target,
            // so the reveal ended with a one-frame jump that read as the panel
            // "pushing" at the end of the motion.
            //
            // Growing later (new rows appearing) still works: rebuildTable()
            // raises the pin when the content needs more room.
        }
        // The table was skipped during the tween; bring it up to date once. This
        // also re-measures the height pin against the REAL content - the target used
        // for the tween was measured against the empty state, which is why the first
        // open could settle at the wrong height.
        m_animating = false;
        refresh();
        if (m_animHeight >= 0) {
            const int want = qMax(120, sizeHint().height());
            if (want != m_animHeight)
                setAnimHeight(want);
        }
        // The tween reached completion: the scroll range is now final, so this is
        // the first moment a viewport glide can be computed correctly.
        emit revealFinished();
    });

    // Ranking refresh, deliberately slower than the value refresh.
    m_rankTimer = new QTimer(this);
    m_rankTimer->setInterval(kRankIntervalMs);
    connect(m_rankTimer, &QTimer::timeout, this, [this] {
        if (m_expanded)
            refresh();
    });
    m_rankTimer->start();

    setVisible(false);
}

void AppActivityPanel::setAnimHeight(int height)
{
    if (m_animHeight == height)
        return;
    m_animHeight = height;
    if (height < 0) {
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        // Both, deliberately: setting only maximumHeight lets the layout keep
        // re-flowing, which is the behaviour being fixed here.
        setMinimumHeight(height);
        setMaximumHeight(height);
        // Announce the step so the viewport can follow the growth in the same
        // frame. Same source, same easing, same instant: the scroll ends up with
        // the height delta exactly once, which is why the motion reads as one.
        emit revealTick(height);
    }
}

void AppActivityPanel::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;

    if (expanded) {
        // Re-expanding cancels any stop that was queued by the last collapse: the
        // session is wanted again, and stopping it now would make the panel sit
        // empty until the next start finished.
        m_stopMonitorWhenCollapsed = false;

        // Monitoring starts only when the panel is first shown, so a collapsed
        // dashboard costs nothing. Asynchronous: even a wedged ETW leftover can
        // hold the bounded cleanup workers, never this panel.
        if (m_monitor && !m_monitor->isRunning())
            m_monitor->startAsync();

        setVisible(true);
        if (m_animated) {
            // Reveal: the panel's height grows from nothing to its natural size.
            //
            // The target must be the height the panel will SETTLE at, or the reveal
            // travels too far and the correction then snaps back - a step at the end
            // of the animation.
            //
            // Measuring sizeHint() directly is not that number: a widget that is
            // hidden still contributes to its layout's size hint. Measured while
            // collapsed, this panel reports 514px because the (hidden) table brings
            // its 160px minimum and 192px hint with it, yet the same panel settles at
            // 188px once expanded with no rows to show. That 326px difference was the
            // end-of-reveal jump.
            //
            // updateStateVisibility() is what rebuildTable() uses to decide which
            // children apply, so applying it first makes sizeHint() describe the
            // state about to be shown. A floor of 120px keeps a momentarily-empty
            // panel from collapsing to a sliver mid-reveal.
            updateStateVisibility();
            const int target = qMax<int>(120, sizeHint().height());

            m_animating = true;
            m_expandAnim->stop();
            m_expandAnim->setStartValue(1);
            m_expandAnim->setEndValue(target);
            // Jump straight to the collapsed height so the first frame is already
            // correct, then animate outwards.
            setAnimHeight(1);

            m_expandAnim->start();
        } else {
            setAnimHeight(-1);
        }
    } else {
        // Collapsing. Brief #10: release the session when it is not needed, so a
        // collapsed dashboard holds no ETW handles.
        //
        // The stop is DEFERRED until the collapse animation has finished, and this
        // is a crash fix, not tidiness. AppMonitor::stop() is synchronous and
        // blocks on the UI thread for up to about a second while the trace session
        // tears down. Calling it here meant that, for the whole 280 ms of the
        // collapse, a blocking teardown was running against the same widgets the
        // animation was still touching - and the panel hides itself at the end of
        // that animation. Show Less crashed the app as a result.
        //
        // Deferring keeps the two apart: the animation completes (or is
        // interrupted), and only then does the session go away. A pending stop is
        // recorded so a rapid expand-collapse-expand cannot leave the session
        // orphaned.
        if (m_monitor && m_monitor->isRunning()) {
            if (m_animated && m_expandAnim
                && m_expandAnim->state() == QAbstractAnimation::Running) {
                m_stopMonitorWhenCollapsed = true;
            } else {
                // No animation in flight: safe to stop immediately.
                m_monitor->stop();
                m_stopMonitorWhenCollapsed = false;
            }
        }

        if (m_animated) {
            // Collapse: shrink from the current height back to nothing. Starting
            // from the measured current height (rather than from the widget's
            // pinned value) keeps the motion continuous if the user toggles again
            // while a reveal is still running.
            //
            // Collapse is the same single motion in reverse: the height clips the
            // content away and nothing else moves. There is no fade to retire and no
            // settle offset to reset - the reveal runs no effects at all.
            m_animating = true;
            m_expandAnim->stop();
            m_expandAnim->setStartValue(qMax(1, height()));
            m_expandAnim->setEndValue(1);
            m_expandAnim->start();
        } else {
            setVisible(false);
            setAnimHeight(-1);
        }
    }

    refresh();
    emit expandedChanged(m_expanded);
}

void AppActivityPanel::refresh()
{
    if (!m_monitor)
        return;

    // Re-parenting destroys the graphics effect.
    //
    // QWidget::setParent() deletes any QGraphicsEffect the widget has, and this
    // panel IS re-parented: ResponsivePair::embedMiddle()/takeMiddle() move it
    // between layouts whenever the window crosses the layout breakpoint. The
    // fading animation kept pointing at the effect that had been deleted, so the
    // next animation step dereferenced freed memory - the access violation inside
    // (The panel no longer uses a QGraphicsOpacityEffect: it forced an offscreen
    // render of the whole subtree - table included - on every animation frame,
    // which was the stutter. Nothing to recover here now.)

    const MonitorState state = m_monitor->state();

    // A failed backend is reported as such. Showing an empty list here would be
    // indistinguishable from "nothing is using the network right now".
    if (state == MonitorState::Unavailable) {
        m_errorText->setText(m_monitor->errorString());
        m_errorState->setVisible(true);
        m_table->setVisible(false);
        m_emptyState->setVisible(false);
        m_viewAllButton->setVisible(false);
        m_unattributedLabel->clear();
        if (m_scopeNoteLabel)
            m_scopeNoteLabel->setVisible(false);
        return;
    }

    m_errorState->setVisible(false);

    const QVector<AppTraffic> apps = m_monitor->snapshot();

    // While the session is running but has not produced an interval yet, keep
    // whatever was last shown rather than flickering to the empty state.
    if (!apps.isEmpty() || m_hasData)
        m_hasData = m_hasData || !apps.isEmpty();

    QVector<AppTraffic> visible;
    for (const AppTraffic &a : apps) {
        if (a.totalBytesPerSec() > 0)
            visible.append(a);
    }

    // Anti-flicker persistence: keep recently active applications visible for a
    // few idle refreshes before dropping them (bursty traffic stops rows from
    // blinking in and out every second).
    auto keyOf = [](const AppTraffic &a) {
        return a.executablePath.isEmpty() ? a.displayName : a.executablePath;
    };
    QSet<QString> busy;
    for (const AppTraffic &a : visible)
        busy.insert(keyOf(a));

    QMutableHashIterator<QString, QPair<int, AppTraffic>> pit(m_persist);
    while (pit.hasNext()) {
        pit.next();
        const QString k = pit.key();
        if (busy.contains(k)) {
            m_persist[k].first = 0;
            continue;
        }
        if (++m_persist[k].first > 4) {
            pit.remove();
        } else if (m_persist[k].second.totalBytesPerSec() > 0
                   || m_persist[k].second.bytesIn > 0) {
            AppTraffic idle = m_persist[k].second;
            idle.downBytesPerSec = 0.0;
            idle.upBytesPerSec = 0.0;
            idle.pids.clear();
            visible.append(idle);
        }
    }
    for (const AppTraffic &a : visible) {
        QPair<int, AppTraffic> &entry = m_persist[keyOf(a)];
        entry.first = 0;
        entry.second = a;
    }

    // Ranking order is taken from the freshly sorted list; row VALUES are updated
    // in place by rebuildTable() so the ordering stays stable between rank ticks.
    m_ranked = visible;

    // Sorting by a column the user chose overrides the bandwidth default.
    if (m_sortBy != SortBy::Bandwidth) {
        std::sort(m_ranked.begin(), m_ranked.end(), [this](const AppTraffic &a,
                                                           const AppTraffic &b) {
            int cmp = 0;
            switch (m_sortBy) {
            case SortBy::Name:
                cmp = QString::compare(a.displayName, b.displayName, Qt::CaseInsensitive);
                break;
            case SortBy::Download:
                cmp = a.downBytesPerSec < b.downBytesPerSec
                          ? -1
                          : (a.downBytesPerSec > b.downBytesPerSec ? 1 : 0);
                break;
            case SortBy::Upload:
                cmp = a.upBytesPerSec < b.upBytesPerSec
                          ? -1
                          : (a.upBytesPerSec > b.upBytesPerSec ? 1 : 0);
                break;
            case SortBy::Bandwidth:
                break;
            }
            return m_sortDescending ? cmp > 0 : cmp < 0;
        });
    } else if (!m_sortDescending) {
        std::reverse(m_ranked.begin(), m_ranked.end());
    }

    rebuildTable();

    // Keep the height pin in step with the CONTENT, in both directions.
    //
    // Raising only was the bug: the reveal target is measured before any rows exist,
    // against the empty state, so on the first open the pin could be set too large -
    // and a guard that only raises can never correct it. The panel then stayed
    // oversized until a collapse and re-expand re-measured it, which is exactly the
    // "first time it is very long, fine afterwards" the user reported.
    //
    // While the reveal is running the tween owns the height, so corrections are
    // deferred to it; afterwards the pin tracks the content either way.
    if (!m_animating && m_animHeight >= 0) {
        const int want = qMax(120, sizeHint().height());
        if (want != m_animHeight)
            setAnimHeight(want);
    }
}


void AppActivityPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    // The columns divide the available width, so a new width means new widths for them.
    // Guarded against re-entering: setting a column width triggers a layout pass, and this is
    // called from the resize that results.
    refreshColumnWidths();
}

void AppActivityPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    // The widths are computed from the table's width, and the table does not exist to be
    // measured until it is visible.
    //
    // resizeEvent cannot cover this: expanding the panel does not necessarily resize it, so a
    // window that never changes size would keep whatever widths were set the first time - which
    // were measured while the panel was collapsed and had almost no width at all. That is
    // exactly what produced the floor values at every window size.
    //
    // Deferred by one event-loop turn because the layout that gives the table its real width
    // has not run at the moment showEvent is delivered.
    QTimer::singleShot(0, this, [this] { refreshColumnWidths(); });
}

void AppActivityPanel::refreshColumnWidths()
{
    if (m_adjustingColumns || !m_table || !m_table->isVisible())
        return;

    m_adjustingColumns = true;
    applyNumericColumnWidths();
    m_adjustingColumns = false;

    // A second pass, because the first may change the layout: a column narrowed below its
    // content can make the table's own width hint change, and the widths are computed from that
    // width. Repeating once settles it rather than leaving the first pass's guess in place.
    if (!m_adjustingColumns && m_table->isVisible()) {
        m_adjustingColumns = true;
        applyNumericColumnWidths();
        m_adjustingColumns = false;
    }
}

void AppActivityPanel::applyNumericColumnWidths()
{
    if (!m_table)
        return;

    // The worst case each column must fit: the heading, and the widest value the
    // rate formatter can produce. Measured from the table's own font so a theme or
    // DPI change cannot make them wrong.
    const QFontMetrics fm(m_table->font());
    const int padding = 16;   // item/header padding (6px each side) plus breathing room

    const auto widthFor = [&](const QString &heading, const QString &sample) {
        return qMax(fm.horizontalAdvance(heading), fm.horizontalAdvance(sample)) + padding;
    };

    // "999.99 MB/s" is the widest realistic output of the rate formatter; a larger
    // value becomes GB/s, which is the same character count.
    const QString sample = QStringLiteral("999.99 MB/s");
    const int download = widthFor(QStringLiteral("Download"), sample);
    const int upload = widthFor(QStringLiteral("Upload"), sample);
    const int total = widthFor(QStringLiteral("Total"), sample);

    // The width to divide is the table's own, not the viewport's.
    //
    // viewport()->width() is correct only once the table has been laid out, and this runs from
    // resizeEvent - which fires BEFORE the new geometry has reached the viewport. Reading it
    // there gave the previous - usually much smaller - width, so the columns were sized for a
    // narrow table and never grew. The table's own width is already the new one at this point,
    // minus the vertical scrollbar the table reserves.
    const int available =
        qMax(0, m_table->width() - m_table->verticalScrollBar()->sizeHint().width() - 2);

    const int wanted = download + upload + total;
    const int nameMinimum = fm.horizontalAdvance(QStringLiteral("Application")) + padding;

    if (available - wanted >= nameMinimum) {
        // Room for everything at the width each column asked for.
        m_table->setColumnWidth(1, download);
        m_table->setColumnWidth(2, upload);
        m_table->setColumnWidth(3, total);
        // The name column takes exactly what is left: enough to fill the row without leaving a
        // sliver at the right edge, and not more than it needs.
        m_table->setColumnWidth(0, available - wanted);
    } else {
        // Not enough room for all four at their wanted widths.
        //
        // The name column is honoured first, at its own minimum, because an application name
        // that cannot be read makes the row useless - the numbers beside it mean nothing without
        // knowing which application they belong to.
        //
        // The numeric columns then share what remains in proportion to what each asked for, so
        // the wider ones keep the most and none is starved. The floor keeps a rate readable
        // rather than reducing it to an ellipsis.
        const int share = qMax(0, available - nameMinimum);
        const int totalDemand = download + upload + total;
        if (totalDemand > 0 && share > 0) {
            m_table->setColumnWidth(1, qMax(kMinColumnWidth, share * download / totalDemand));
            m_table->setColumnWidth(2, qMax(kMinColumnWidth, share * upload / totalDemand));
            m_table->setColumnWidth(3, qMax(kMinColumnWidth, share * total / totalDemand));
        }
        // Whatever is genuinely left goes to the name column. Taking more would push the numeric
        // columns past the edge, which is the fault being fixed.
        int numeric = 0;
        for (int c = 1; c <= 3; ++c)
            numeric += m_table->columnWidth(c);
        m_table->setColumnWidth(0, qMax(nameMinimum, available - numeric));
    }

    // Elide rather than overflow. A column too narrow to show its value whole then ends in an
    // ellipsis, which reads as "there is more here" instead of drawing text outside the cell.
    m_table->setTextElideMode(Qt::ElideRight);

}

void AppActivityPanel::updateStateVisibility()
{
    // Make the panel's reported size hint describe the state it is about to show.
    //
    // A hidden child still counts toward its layout's size hint, so the collapsed
    // panel reported a height that included the (hidden) table - 514px against the
    // 188px it actually settles at. Applying the same visibility rules rebuildTable()
    // uses, BEFORE measuring, makes the reveal target honest.
    if (!m_table || !m_emptyState || !m_errorState)
        return;

    const bool failed = m_monitor && m_monitor->state() == MonitorState::Unavailable;
    const bool hasRows = !m_ranked.isEmpty();

    m_errorState->setVisible(failed);
    m_table->setVisible(!failed && hasRows);
    if (!failed && hasRows) {
        // The table has just become visible, which is the first moment its width is real.
        // Deferred, because the layout that gives it that width has not run yet.
        QTimer::singleShot(0, this, [this] { refreshColumnWidths(); });
    }
    m_emptyState->setVisible(!failed && !hasRows);
    if (m_viewAllButton)
        m_viewAllButton->setVisible(!failed && hasRows && m_ranked.size() > kTopCount);
}

void AppActivityPanel::rebuildTable()
{
    const bool hasRows = !m_ranked.isEmpty();

    m_emptyState->setVisible(!hasRows);
    m_table->setVisible(hasRows);
    if (!hasRows) {
        m_viewAllButton->setVisible(false);
        m_unattributedLabel->clear();
        if (m_scopeNoteLabel)
            m_scopeNoteLabel->setVisible(false);

        // With the asynchronous start the panel can be shown a heartbeat before
        // the session is settled: say so instead of flashing "nothing is using
        // the network" on the way to a possible error message.
        if (m_emptyTitle) {
            const bool starting =
                m_monitor && m_monitor->isRunning() && m_monitor->isStarting();
            m_emptyTitle->setText(
                starting ? QStringLiteral("Starting monitoring")
                         : QStringLiteral("No active network applications"));
            // A moving indicator while the session settles, so a wait is visibly a
            // wait rather than a still image that looks like a finished, empty
            // result. Hidden as soon as there are rows or a real error.
            if (m_busyDots) {
                m_busyDots->setVisible(starting);
                if (starting)
                    m_busyDots->setLabel(QStringLiteral("waiting for the first reading"));
            }
        }
        return;
    }

    // Rows are being shown: the waiting indicator must not linger.
    if (m_busyDots)
        m_busyDots->setVisible(false);

    const int shown = m_showAll ? m_ranked.size() : qMin(kTopCount, m_ranked.size());
    const bool more = m_ranked.size() > kTopCount;
    m_viewAllButton->setVisible(more);
    m_viewAllButton->setText(m_showAll
                                 ? QStringLiteral("Show Top %1").arg(kTopCount)
                                 : QStringLiteral("View All Applications (%1)")
                                       .arg(m_ranked.size()));

    m_table->setRowCount(shown);

    for (int row = 0; row < shown; ++row) {
        const AppTraffic &a = m_ranked.at(row);

        // Name cell carries the icon. Icons are cached by appnames::appIcon, so
        // this does not re-extract per refresh.
        auto *nameItem = makeItem(a.displayName, Qt::AlignLeft);
        nameItem->setIcon(appIcon(a.executablePath, a.executableName));
        nameItem->setToolTip(a.pids.isEmpty()
                                 ? a.displayName
                                 : QStringLiteral("%1\n%2 process(es)")
                                       .arg(a.executableName.isEmpty() ? a.displayName
                                                                       : a.executableName)
                                       .arg(a.pids.size()));
        m_table->setItem(row, 0, nameItem);

        m_table->setItem(row, 1, makeItem(rateText(a.downBytesPerSec), Qt::AlignRight));
        m_table->setItem(row, 2, makeItem(rateText(a.upBytesPerSec), Qt::AlignRight));
        m_table->setItem(row, 3, makeItem(rateText(a.totalBytesPerSec()), Qt::AlignRight));
    }

    // Unattributed traffic is reported separately rather than folded into an
    // application, and only when there is any.
    const AppTraffic un = m_monitor ? m_monitor->unattributed() : AppTraffic{};
    if (un.totalBytesPerSec() > 0) {
        m_unattributedLabel->setText(
            QStringLiteral("System and unattributed: %1 \u2193  %2 \u2191")
                .arg(rateText(un.downBytesPerSec), rateText(un.upBytesPerSec)));
    } else {
        m_unattributedLabel->clear();
    }

    // Scope note: shown only while the numbers come from the TCP-only fallback,
    // so a lower reading is explained instead of looking like a measurement bug.
    if (m_scopeNoteLabel) {
        const bool fallback = m_monitor && m_monitor->usingTcpFallback();
        if (fallback && m_monitor) {
            const QString reason = m_monitor->fallbackReason();
            if (!reason.isEmpty()) {
                m_scopeNoteLabel->setText(reason);
                m_scopeNoteLabel->setToolTip(reason);
            }
        }
        m_scopeNoteLabel->setVisible(fallback);
    }
}

void AppActivityPanel::sortByColumn(int column)
{
    SortBy next = m_sortBy;
    switch (column) {
    case 0:
        next = SortBy::Name;
        break;
    case 1:
        next = SortBy::Download;
        break;
    case 2:
        next = SortBy::Upload;
        break;
    default:
        next = SortBy::Bandwidth;
        break;
    }

    if (next == m_sortBy) {
        // Clicking the same column reverses the direction.
        m_sortDescending = !m_sortDescending;
    } else {
        m_sortBy = next;
        // Numeric columns start descending (largest first); the name column
        // starts ascending, which is what a reader expects.
        m_sortDescending = (next != SortBy::Name);
    }

    refresh();
}

} // namespace net
