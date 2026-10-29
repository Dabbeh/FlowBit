#include "appactivitypanel.h"

#include "appnames.h"
#include "busydots.h"
#include "theme.h"

#include <QFrame>
#include <QEasingCurve>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QResizeEvent>
#include <QShowEvent>
#include <QScrollBar>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QScreen>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <windows.h>
#include <mmsystem.h>

namespace net {

namespace {

// The narrowest a numeric column may be squeezed. Below this a rate is unreadable even elided,
// so it is a floor for legibility rather than for the arithmetic.
constexpr int kMinColumnWidth = 56;
constexpr int kRevealDurationMs = 360;

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


// Reveal and settle timing.
//
// 280ms is the reveal: InOutSine starts at zero velocity, so a shorter duration reads as a
// hesitation before the panel moves at all. The settle is deliberately much shorter - it is
// a correction, not a second movement, and anything longer would read as two motions.
// 340ms rather than 280ms: the reveal has to travel ~190-500px depending on row count, and
// at 280ms the fastest frames stepped 17px each - measured - which reads as chunky rather
// than smooth. A longer duration does not change the curve's shape, only how many frames
// share the distance.
constexpr int kRevealMs = 340;
constexpr int kSettleMs = 140;
} // namespace

AppActivityPanel::AppActivityPanel(AppMonitor *monitor, QWidget *parent)
    : QFrame(parent), m_monitor(monitor)
{
    setObjectName(QStringLiteral("card"));
    setStyleSheet(theme::cardStyleSheet(QStringLiteral("card")));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    policy.setRetainSizeWhenHidden(true);
    setSizePolicy(policy);
    setFixedHeight(0);

    // Keep the table, labels and footer at their full layout size. Only this
    // frame's clipping height changes during a reveal.
    m_content = new QWidget(this);
    auto *root = new QVBoxLayout(m_content);
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

    // Switching between waiting, error and measured data keeps one stable body
    // size rather than starting another height change after the reveal ends.
    m_states = new QStackedWidget(m_content);
    m_states->setMinimumHeight(180);
    root->addWidget(m_states, 1);

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

    m_states->addWidget(m_table);

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

    m_states->addWidget(m_emptyState);

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

    m_states->addWidget(m_errorState);
    m_states->setCurrentWidget(m_emptyState);

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
    // The visible line says WHAT the table is measuring; the tooltip says WHY, because the answer
    // is a state the user cannot see from here: the detailed source is silent, so Flowbit is on the
    // TCP-counter fallback. Without this the note reads as a permanent limitation of the app rather
    // than as a description of the source currently in use.
    m_scopeNoteLabel->setToolTip(
        QStringLiteral("The detailed ETW source has delivered no events in this session, so "
                       "per-application figures come from Windows' TCP connection counters. Those "
                       "counters have no UDP data at all.\n\nSettings \u2192 Diagnostics shows the "
                       "ETW consumer state and which event ids arrived - that is where to look if "
                       "this note stays visible while traffic is flowing."));
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

    // The outer frame clips a stable inner layout; its children never compress
    // into the intermediate heights of the animation.
    // QWidget property animations normally advance every ~16 ms. A card this
    // tall then moves more than 20 pixels per update. Give this short reveal
    // its own precise clock so it also paints smoothly on faster displays.
    m_revealTimer = new QTimer(this);
    m_revealTimer->setTimerType(Qt::PreciseTimer);
    connect(m_revealTimer, &QTimer::timeout, this, &AppActivityPanel::advanceReveal);

    // Ranking refresh, deliberately slower than the value refresh.
    m_rankTimer = new QTimer(this);
    m_rankTimer->setInterval(kRankIntervalMs);
    connect(m_rankTimer, &QTimer::timeout, this, [this] {
        // Expanded AND on screen. The timer used to rebuild the table while the panel was
        // collapsed out of sight or while another page was showing, which cost a re-sort and a
        // full row rebuild every three seconds for output nobody could see.
        if (m_expanded && isVisible() && !m_animating)
            refresh();
    });
    m_rankTimer->start();

    setVisible(false);
}

AppActivityPanel::~AppActivityPanel()
{
    m_revealTimer->stop();
    endPreciseTiming();
}

void AppActivityPanel::beginPreciseTiming()
{
    if (!m_preciseTiming && timeBeginPeriod(1) == TIMERR_NOERROR)
        m_preciseTiming = true;
}

void AppActivityPanel::endPreciseTiming()
{
    if (m_preciseTiming) {
        timeEndPeriod(1);
        m_preciseTiming = false;
    }
}

int AppActivityPanel::contentHeight() const
{
    return m_contentHeight;
}

QSize AppActivityPanel::sizeHint() const
{
    return QSize(m_content->sizeHint().width(), contentHeight());
}

QSize AppActivityPanel::minimumSizeHint() const
{
    return QSize(m_content->minimumSizeHint().width(), 0);
}

void AppActivityPanel::layoutContent()
{
    const int wrapped = m_content->heightForWidth(width());
    const int natural = qMax(240, wrapped >= 0 ? wrapped : m_content->sizeHint().height());
    // Data arriving after a reveal must not make the card bounce smaller.
    m_contentHeight = m_expanded ? qMax(m_contentHeight, natural) : natural;
    m_content->setFixedHeight(m_contentHeight);
    const QRect geometry(0, 0, width(), m_contentHeight);
    if (m_content->geometry() != geometry)
        m_content->setGeometry(geometry);
    m_content->layout()->activate();
}

void AppActivityPanel::finishReveal()
{
    m_revealTimer->stop();
    endPreciseTiming();
    m_animating = false;
    if (!m_expanded) {
        setAnimHeight(0);
        hide();
    }
    emit revealFinished();
    if (m_expanded) {
        QTimer::singleShot(0, this, [this] {
            if (m_expanded && !m_animating)
                refresh();
        });
    }
}

void AppActivityPanel::animateToHeight(int target)
{
    const int from = qMax(0, m_animHeight >= 0 ? m_animHeight : height());
    // Reverse the existing timeline when possible. Restarting an easing curve
    // on each click would insert a pause and discard its current progress.
    if (m_animated && m_animating && (target == m_revealFrom || target == m_revealTo)) {
        const int direction = target == m_revealTo ? 1 : -1;
        if (direction == m_revealDirection)
            return;
        m_revealDirection = direction;
        m_revealClock.restart();
        emit revealStarted(from, target);
        return;
    }
    m_revealTimer->stop();
    if (!m_animated || from == target) {
        const bool settling = m_animating;
        m_animating = false;
        endPreciseTiming();
        setAnimHeight(target);
        if (!m_expanded) {
            hide();
        }
        if (settling)
            emit revealFinished();
        return;
    }
    m_animating = true;
    m_revealFrom = from;
    m_revealTo = target;
    m_revealDirection = 1;
    m_revealTimeMs = 0;
    m_revealDurationMs = qBound(160, kRevealDurationMs * qAbs(target - from) / contentHeight(),
                               kRevealDurationMs);
    emit revealStarted(from, target);
    // Oversample 60 Hz screens and follow faster screens up to 240 Hz. This
    // clock is local to the panel and stops completely when the reveal ends.
    const qreal rate = qBound(120.0, screen() ? screen()->refreshRate() : 120.0, 240.0);
    beginPreciseTiming();
    m_revealClock.start();
    m_revealTimer->start(qMax(4, qRound(1000.0 / rate)));
}

void AppActivityPanel::advanceReveal()
{
    const qreal elapsedMs = m_revealClock.nsecsElapsed() / 1000000.0;
    m_revealClock.restart();
    m_revealTimeMs = qBound(0.0, m_revealTimeMs + m_revealDirection * elapsedMs,
                           qreal(m_revealDurationMs));
    const qreal progress = QEasingCurve(QEasingCurve::InOutSine)
                               .valueForProgress(m_revealTimeMs / m_revealDurationMs);
    setAnimHeight(qRound(m_revealFrom + (m_revealTo - m_revealFrom) * progress));
    if ((m_revealDirection > 0 && m_revealTimeMs >= m_revealDurationMs)
        || (m_revealDirection < 0 && m_revealTimeMs <= 0))
        finishReveal();
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
        setFixedHeight(height);
        emit revealTick(height);
    }
}

void AppActivityPanel::setExpanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    // Preserve the current visible extent when reversing a running transition.
    const int from = isVisible() ? height() : 0;
    if (expanded && !m_animating) {
        if (m_monitor && !m_monitor->isRunning())
            m_monitor->startAsync();
        refresh();
    }
    if (expanded) {
        setAnimHeight(from);
        // show() delivers any resize event accumulated while the panel was
        // hidden. Keep the collapsed state until that event has been handled;
        // otherwise resizeEvent pins the frame to its full height before the
        // animation even gets its starting value.
        show();
        m_expanded = true;
        if (!m_animating)
            layoutContent();
        animateToHeight(contentHeight());
    } else {
        m_expanded = false;
        // Monitoring continues while collapsed so usage accounting stays live.
        animateToHeight(0);
    }
    emit expandedChanged(m_expanded);
}

void AppActivityPanel::refresh()
{
    if (!m_monitor || m_animating)
        return;

    const MonitorState state = m_monitor->state();

    // A failed backend is reported as such. Showing an empty list here would be
    // indistinguishable from "nothing is using the network right now".
    if (state == MonitorState::Unavailable) {
        m_errorText->setText(m_monitor->errorString());
        m_states->setCurrentWidget(m_errorState);
        m_viewAllButton->setVisible(false);
        m_unattributedLabel->clear();
        if (m_scopeNoteLabel)
            m_scopeNoteLabel->setVisible(false);
        layoutContent();
        if (m_expanded && isVisible())
            animateToHeight(contentHeight());
        return;
    }

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

    layoutContent();
    if (m_expanded && isVisible())
        animateToHeight(contentHeight());
}


void AppActivityPanel::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    // Height-only animation frames never resize or remeasure the table.
    if (event->size().width() == event->oldSize().width())
        return;
    if (m_animating) {
        layoutContent();
        refreshColumnWidths();
        if (m_expanded)
            animateToHeight(contentHeight());
    } else {
        layoutContent();
        refreshColumnWidths();
        if (m_expanded)
            setAnimHeight(contentHeight());
    }
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

void AppActivityPanel::rebuildTable()
{
    const bool hasRows = !m_ranked.isEmpty();

    m_states->setCurrentWidget(hasRows ? static_cast<QWidget *>(m_table) : m_emptyState);
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
