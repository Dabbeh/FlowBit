#include "mainwindow.h"

#include <QThread>

#include "appactivitypanel.h"
#include "diagnosticspanel.h"
#include "applimitengine.h"
#include "applimiterpage.h"
#include "updater.h"
#include "autostart.h"
#include <QCheckBox>
#include <QDebug>
#include <QStandardItemModel>
#include <QStyle>
#include "appruledialog.h"
#include "appmonitor.h"
#include "appnames.h"
#include "brandmark.h"
#include "daypicker.h"
#include "rateeditor.h"
#include "rulerow.h"
#include "schedule.h"
#include "segmentedbar.h"
#include "responsivepair.h"
#include "schedulecard.h"
#include "scheduledialog.h"
#include "crossticon.h"
#include "scrollbaranimator.h"
#include "settingsstore.h"
#include "sidebar.h"
#include "speedgraph.h"
#include "speedmonitor.h"
#include "theme.h"
#include "throttleengine.h"
#include "toggleswitch.h"
#include "trackslider.h"
#include "units.h"

#include <QFile>
#include <QFileInfo>
#include "usagetracker.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainterPath>
#include <QMessageBox>
#include <QMenu>
#include <QPainter>
#include <QPolygonF>
#include <QProcess>
#include <QPushButton>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QPropertyAnimation>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace net {

namespace {

constexpr int kScheduleTickMs = 30 * 1000;   // re-evaluate rules twice a minute
constexpr int kUsageFlushMs = 30 * 1000;     // checkpoint usage every 30 seconds
constexpr int kSliderSteps = 1000;           // slider resolution

// Slider scale: 0 => uncapped, then a piecewise ramp. The first 60% of travel
// covers 0-2 MB/s (where fine control matters); the rest reaches 100 MB/s so the
// control stays useful on very fast links.
constexpr double kLowBandBytes = 2.0 * 1024 * 1024;
constexpr double kHighBandBytes = 100.0 * 1024 * 1024;
constexpr double kLowBandFraction = 0.60;

// Preset quick actions on the limiter card.
struct Preset {
    const char *label;
    double downBytes;
    double upBytes;
};

const Preset kPresets[] = {
    {"Gaming Mode", 1.5 * 1024 * 1024, 512.0 * 1024},
    {"Night Cap", 4.0 * 1024 * 1024, 1.0 * 1024 * 1024},
    {"Work Hours", 2.5 * 1024 * 1024, 500.0 * 1024},
};

} // namespace

MainWindow::MainWindow(SettingsStore *store, ThrottleEngine *engine, SpeedMonitor *monitor,
                       UsageTracker *usage, QWidget *parent)
    : QMainWindow(parent), m_store(store), m_engine(engine), m_monitor(monitor), m_usage(usage)
{
    setWindowTitle(QStringLiteral("Flowbit"));

    // Per-application monitoring. Created up front but not STARTED: the trace
    // session only begins when the Application Network Activity panel is expanded,
    // so a user who never opens it pays no cost. The monitor owns its own thread.
    m_appMonitor = new AppMonitor(this);
    connect(m_appMonitor, &AppMonitor::updated, this, [this] {
        // A completed interval is recorded exactly once, including intervals
        // lengthened by a UI transition. Reading it from the speed timer could
        // record the same snapshot again while the app-monitor tick was deferred.
        for (const AppTraffic &traffic : m_appMonitor->snapshot()) {
            const QString path = traffic.executablePath.isEmpty() ? traffic.executableName
                                                                  : traffic.executablePath;
            if (!path.isEmpty())
                m_usage->addAppSample(path,
                    traffic.displayName.isEmpty() ? traffic.executableName : traffic.displayName,
                    traffic.bytesIn, traffic.bytesOut);
        }
    });

    // Per-application limiting. Also created up front and started only once the
    // user has rules, so a machine with none never pays for the driver handles.
    m_appLimitEngine = new AppLimitEngine(this);
    // 850x580 minimum: at 550 the Live Speed card clamped its graph and the
    // scheduler rows collided with the helper text. 600 is the comfortable
    // working height, with 580 as the floor.
    // Tall enough for the whole four-zone layout to be laid out without clipping.
    // The scheduler card's own minimumSizeHint is ~208px (header, a 100px list,
    // helper text and footer); at 580 the window gave it 177px, so seven children
    // overflowed the card. 650 puts every card comfortably above its minimum at
    // the smallest allowed size, in both themes.
    setMinimumSize(880, 650);
    resize(880, 620);

    buildUi();
    buildTray();
    loadFromStore();

    connect(m_engine, &ThrottleEngine::errorOccurred, this, &MainWindow::onEngineError);
    connect(m_engine, &ThrottleEngine::stateChanged, this, &MainWindow::onEngineStateChanged);
    connect(m_monitor, &SpeedMonitor::sampled, this, &MainWindow::onThroughput);
    connect(m_monitor, &SpeedMonitor::primaryInterfaceChanged, this,
            [this](const QString &adapter) {
                // SpeedMonitor re-baselined its own counters on the switch, so
                // nothing is recorded for usage this tick - correct, because the
                // new adapter's cumulative totals are not new traffic.
                statusBar()->showMessage(
                    QStringLiteral("Now measuring adapter: %1").arg(adapter), 4000);
            });
    connect(m_usage, &UsageTracker::usageChanged, this, &MainWindow::onUsageChanged);

    m_scheduleTimer = new QTimer(this);
    m_scheduleTimer->setInterval(kScheduleTickMs);
    connect(m_scheduleTimer, &QTimer::timeout, this, &MainWindow::onEvaluate);
    m_scheduleTimer->start();

    m_applyDebounce = new QTimer(this);
    m_applyDebounce->setSingleShot(true);
    m_applyDebounce->setInterval(150);
    connect(m_applyDebounce, &QTimer::timeout, this, &MainWindow::persistAndApply);

    // Usage totals live in memory and are flushed periodically, so a hard power
    // loss costs at most a minute of accounting.
    m_usageFlushTimer = new QTimer(this);
    m_usageFlushTimer->setInterval(kUsageFlushMs);
    connect(m_usageFlushTimer, &QTimer::timeout, this, [this] { m_usage->flush(); });
    m_usageFlushTimer->start();

    // Pulls a fresh snapshot into the app panel once a second, but only while it
    // is expanded. The monitor's own thread does the measuring; this timer just
    // moves the results into widgets, so it never blocks on the trace session.
    m_appRefreshTimer = new QTimer(this);
    m_appRefreshTimer->setInterval(1000);
    connect(m_appRefreshTimer, &QTimer::timeout, this, [this] {
        // OS counter polling and row updates share the animation's GUI thread.
        // Bytes keep accumulating; the next tick includes the deferred interval.
        if (m_appPanel && m_appPanel->isAnimating())
            return;
        // Advance the monitoring intervals first, then redraw what they produced.
        // The 1s cadence is what keeps the per-application graph ticking; without
        // this driver the ingested bytes never became a finished snapshot.
        // The monitor tick must keep running: it is what turns ingested bytes into finished
        // intervals, and the usage totals depend on it whether or not anything is on screen.
        // Skipped for the ~300ms a tween runs: tick() formats and sorts every application, and
        // doing that between two animation frames is what the measured 20-30ms stalls were.
        // Nothing is lost by waiting - ingest() keeps accumulating under its own lock, and the
        // next tick finalises the same interval.
        const bool tweenRunning = m_appPanel && m_appPanel->isAnimating();
        if (!tweenRunning && m_appMonitor && m_appMonitor->isRunning())
            m_appMonitor->tick();

        // The panel refresh is pure presentation - it re-sorts, re-formats and rebuilds rows.
        // Doing that while Flowbit sits in the tray, or while another page is showing, was work
        // whose only result was thrown away on the next tick.
        if (m_appPanel && m_appPanel->isExpanded() && m_appPanel->isVisible()
            && !m_appPanel->isAnimating())
            m_appPanel->refresh();
    });
    m_appRefreshTimer->start();

    // Monitoring starts with the APPLICATION, not with the panel being expanded.
    //
    // It used to start only when the activity panel was first shown, so that a collapsed
    // dashboard cost nothing. That made the per-application figures wrong by design: the usage
    // table is filled from the monitor's snapshot, so it recorded only traffic that happened
    // while the panel happened to be open.
    //
    // It also produced a diagnostic that could not be explained from the app: "ETW session:
    // works (StartTrace 0)" printed directly above "ETW events received: 0 (decoded: 0)", on a
    // machine that was busy. Both were true - the session was creatable and no session existed
    // - because an administrator sitting in Settings never expands the dashboard panel, so
    // monitoring never began at all.
    //
    // The cost is bounded and already paid for: the ETW schema is cached per event id, and
    // aggregation is one hash update per event.
    if (m_appMonitor)
        m_appMonitor->startAsync();

    // Per-application schedule windows open and close on wall-clock time, so the
    // active set is recomputed periodically. The interval is coarse on purpose: a
    // window boundary is a minute-resolution event, and this must cost nothing
    // measurable while a game is running.
    m_appLimitTimer = new QTimer(this);
    m_appLimitTimer->setInterval(30000);
    connect(m_appLimitTimer, &QTimer::timeout, this, [this] {
        if (m_appLimitEngine->isEnabled()) {
            m_appLimitEngine->refresh();
            if (m_appLimiterPageWidget)
                m_appLimiterPageWidget->refresh();
        }
    });
    m_appLimitTimer->start();

    m_monitor->start();
    onEvaluate();
    refreshUsage();
}

MainWindow::~MainWindow()
{
    // Leave the machine uncapped and the counters banked on the way out.
    m_engine->detach();
    m_usage->flush();
}

// ---------------------------------------------------------------------------
// Card helper
// ---------------------------------------------------------------------------

QFrame *MainWindow::makeCard(const QString &title, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("card"));
    card->setStyleSheet(theme::cardStyleSheet(QStringLiteral("card")));

    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 11, 14, 13);
    layout->setSpacing(9);

    if (!title.isEmpty()) {
        auto *label = new QLabel(title.toUpper(), card);
        label->setObjectName(QStringLiteral("cardTitle"));
        layout->addWidget(label);
    }
    return card;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Navigation shell
// ---------------------------------------------------------------------------

// Wraps a page's body in the standard heading block the reference shows:
// a title, an optional subheading, and an optional trailing action widget.
QWidget *MainWindow::makePage(const QString &heading, const QString &subheading, QWidget **bodyOut)
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 14, 16, 12);
    outer->setSpacing(12);

    auto *head = new QHBoxLayout();
    head->setSpacing(10);

    auto *titleCol = new QVBoxLayout();
    titleCol->setSpacing(2);

    auto *title = new QLabel(heading, page);
    title->setObjectName(QStringLiteral("pageTitle"));
    titleCol->addWidget(title);

    if (!subheading.isEmpty()) {
        auto *sub = new QLabel(subheading, page);
        sub->setObjectName(QStringLiteral("sub"));
        sub->setWordWrap(true);
        titleCol->addWidget(sub);
    }

    head->addLayout(titleCol);
    head->addStretch(1);
    outer->addLayout(head);

    auto *body = new QWidget(page);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(12);

    outer->addWidget(body, 1);
    if (bodyOut)
        *bodyOut = body;

    return page;
}

// ---------------------------------------------------------------------------
// Per-application limiting
// ---------------------------------------------------------------------------

void MainWindow::onAppRuleAdd()
{
    AppRule fresh;
    AppRuleDialog dialog(fresh, true, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    AppRule rule = dialog.rule();
    // Ids come from the same sequence the schedule rules use, so an id is unique
    // across the whole settings file.
    rule.id = nextRuleId();

    Settings settings = m_store->load();
    settings.appRules.append(rule);
    persistSettings(settings, QStringLiteral("your settings"));
    persistAndApply();
}

void MainWindow::onAppRuleEdit(quint64 id)
{
    Settings settings = m_store->load();
    const int index = appRuleIndexById(settings, id);
    if (index < 0)
        return;

    // The existing rule is passed in, so its id and any schedule windows survive
    // the edit. Rebuilding it from the dialog fields alone would silently drop the
    // windows and, on accept, create a duplicate entry.
    AppRule edited = settings.appRules.at(index);
    AppRuleDialog dialog(edited, false, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    AppRule result = dialog.rule();
    result.id = id;                          // never reassigned on edit
    result.windows = edited.windows;         // owned by the schedule dialog

    settings.appRules[index] = result;
    persistSettings(settings, QStringLiteral("your settings"));
    persistAndApply();
}

void MainWindow::onAppRuleDelete(quint64 id)
{
    Settings settings = m_store->load();
    const int index = appRuleIndexById(settings, id);
    if (index < 0)
        return;

    const QString name = settings.appRules.at(index).name;
    const QString question =
        QStringLiteral("Stop limiting %1?\n\nIts saved limits and schedule will be removed.")
            .arg(name.isEmpty() ? QStringLiteral("this application") : name);

    // Confirmed, because deleting discards the limits and any schedule windows the
    // user configured, and there is no undo.
    if (QMessageBox::question(this, QStringLiteral("Remove application limit"), question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    settings.appRules.remove(index);
    persistSettings(settings, QStringLiteral("your settings"));
    persistAndApply();
}

void MainWindow::onAppRuleEnabledChanged(quint64 id, bool enabled)
{
    Settings settings = m_store->load();
    const int index = appRuleIndexById(settings, id);
    if (index < 0)
        return;

    if (settings.appRules.at(index).enabled == enabled)
        return;

    settings.appRules[index].enabled = enabled;
    persistSettings(settings, QStringLiteral("your settings"));
    persistAndApply();
}

void MainWindow::onAppRuleSchedule(quint64 id)
{
    Settings settings = m_store->load();
    const int index = appRuleIndexById(settings, id);
    if (index < 0)
        return;

    AppRule &rule = settings.appRules[index];

    // One dialog, listing this application's schedule windows so any of them can be
    // edited, and any of them deleted.
    //
    // It opens showing the FIRST window. Previously the handler always did that too, but
    // it also always wrote the result back to windows[0] - so a second window could be
    // added and then never selected again, and neither could be removed. The dialog now
    // carries the list, and the changes below are applied by id so removing or editing one
    // window cannot disturb another.
    const bool haveWindow = !rule.windows.isEmpty();

    Rule start;
    if (haveWindow) {
        start = rule.windows.first();
    } else {
        start.name = rule.name;
        start.down = rule.down;
        start.up = rule.up;
        start.from = QStringLiteral("22:00");
        start.to = QStringLiteral("07:00");
        start.days = 0x7F;   // every day, the usual intent for a first window
    }
    start.id = haveWindow ? start.id : nextRuleId();

    // The application's other windows, so the dialog can offer them. Only this
    // application's: another application's windows are never passed in.
    QVector<Rule> others = rule.windows;
    if (haveWindow) {
        for (int i = 0; i < others.size(); ++i) {
            if (others.at(i).id == start.id) {
                others.removeAt(i);
                break;
            }
        }
    }

    ScheduleDialog dialog(start, !haveWindow, this, others);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // Deletions first, by id.
    //
    // Collected into a set as well as applied, because the step below must know which
    // windows are gone: result() is not cleared by deleting, so it can still name a window
    // that has just been removed, and writing it back would resurrect it.
    QSet<quint64> deleted;
    for (quint64 gone : dialog.removedIds()) {
        deleted.insert(gone);
        for (int i = 0; i < rule.windows.size(); ++i) {
            if (rule.windows.at(i).id == gone) {
                rule.windows.removeAt(i);
                break;
            }
        }
    }

    // The window that was edited, put back where it belongs by id rather than by position -
    // the list may have changed while the dialog was open.
    //
    // Skipped when that window was one the user deleted. Without this check the window is
    // removed above and appended here, so deleting the window the dialog opened with
    // appears to do nothing - which is precisely what was reported.
    const Rule result = dialog.result();
    if (result.id != 0 && !deleted.contains(result.id)) {
        bool replaced = false;
        for (Rule &existing : rule.windows) {
            if (existing.id == result.id) {
                existing = result;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            rule.windows.append(result);
    }

    // Windows started with "+ Add Schedule", each with its own id.
    for (Rule extra : dialog.addedRules()) {
        extra.id = nextRuleId();
        rule.windows.append(extra);
    }

    persistSettings(settings, QStringLiteral("your settings"));
    persistAndApply();
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *shell = new QHBoxLayout(central);
    // The sidebar sits flush to the window edge, the way the reference shows it.
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    m_sidebar = new Sidebar(central);
    connect(m_sidebar, &Sidebar::pageSelected, this, &MainWindow::onPageSelected);
    shell->addWidget(m_sidebar);

    // A thin separator so the rail reads as a distinct surface in both themes.
    auto *divider = new QFrame(central);
    divider->setObjectName(QStringLiteral("sidebarDivider"));
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedWidth(1);
    shell->addWidget(divider);

    auto *content = new QWidget(central);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_pages = new QStackedWidget(content);
    m_dashboardPage = buildDashboardPage();
    m_schedulerPage = buildSchedulerPage();
    m_appLimiterPage = buildAppLimiterPage();
    m_statisticsPage = buildStatisticsPage();
    m_settingsPage = buildSettingsPage();

    // Index order must match Sidebar::Page.
    m_pages->addWidget(m_dashboardPage);
    m_pages->addWidget(m_schedulerPage);
    m_pages->addWidget(m_appLimiterPage);
    m_pages->addWidget(m_statisticsPage);
    m_pages->addWidget(m_settingsPage);
    contentLayout->addWidget(m_pages, 1);

    // The error strip lives below every page, so a problem is visible wherever
    // the user happens to be.
    //
    // Laid out as a card with the dismiss cross pinned to the top-right corner, the way a
    // Windows notification and most alert strips do it. The previous arrangement put the label
    // and the cross side by side in a vertical box, which left the cross floating in the middle
    // of the text column rather than sitting in the corner - it read as a stray control next to
    // the message instead of as the way to close it.
    //
    // A QGridLayout is what makes the corner placement work: the label occupies one cell and
    // stretches, the cross occupies a cell at the top-right and takes only what it needs. A
    // horizontal box would align the two to the same height, which is wrong for a message that
    // wraps to several lines.
    auto *errRow = new QWidget(content);
    errRow->setObjectName(QStringLiteral("errorStrip"));
    auto *errGrid = new QGridLayout(errRow);
    errGrid->setContentsMargins(16, 8, 10, 10);
    errGrid->setHorizontalSpacing(10);
    errGrid->setVerticalSpacing(0);

    // The container is hidden, not just the label inside it.
    //
    // errRow carries the objectName that gives it the card background, the border and the red
    // left accent. Hiding the label left the styled card on screen permanently and empty -
    // reported as an accent sitting at the side "all the time". Hiding the text does not hide
    // the box the text sits in.
    m_errorStrip = errRow;
    m_errorStrip->hide();

    m_errorLabel = new QLabel(errRow);
    m_errorLabel->setObjectName(QStringLiteral("error"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    if (m_errorStrip)
        m_errorStrip->hide();
    // Row 0, column 0: takes the width, and the cross sits in the column beside it.
    errGrid->addWidget(m_errorLabel, 0, 0);
    errGrid->setColumnStretch(0, 1);
    // The cross column takes only its natural width, so the message keeps the rest.
    errGrid->setColumnStretch(1, 0);

    // A way to dismiss it.
    //
    // The banner was set from four places and never hidden again, so a failure that had
    // since been fixed - including the driver failure the engine now repairs on its own
    // - stayed on screen for the rest of the session with no way to clear it.
    // A drawn cross, not a font character.
    //
    // This used the multiplication sign U+2715, which Arial does not contain - and Arial is
    // this application's fallback family - so the button rendered as an empty outlined box.
    // The icon is painted from a path, so it depends on no font at all.
    // The auto-dismiss timer. Restarted whenever the banner's text changes, so the delay is
    // measured from the most recent message rather than from when the banner first appeared.
    // The driver readiness retry. Single-shot, and restarted by its own handler while
    // the driver is still held - which is how a bounded poll is written with a repeating
    // timer's shape: each tick decides whether there is another.
    m_driverRetry = new QTimer(this);
    m_driverRetry->setSingleShot(true);
    connect(m_driverRetry, &QTimer::timeout, this, &MainWindow::onDriverRetry);

    m_errorAutoHide = new QTimer(this);
    m_errorAutoHide->setSingleShot(true);
    m_errorAutoHide->setInterval(6000);
    connect(m_errorAutoHide, &QTimer::timeout, this, [this] {
        // A quick fade rather than vanishing: an element that disappears between two frames
        // reads as a glitch, and this project already has the animation machinery.
        if (!m_errorLabel || !m_errorLabel->isVisible())
            return;
        auto *effect = new QGraphicsOpacityEffect(m_errorLabel);
        m_errorLabel->setGraphicsEffect(effect);
        auto *anim = new QPropertyAnimation(effect, "opacity", m_errorLabel);
        anim->setDuration(400);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(anim, &QPropertyAnimation::finished, this, [this] {
            if (m_errorLabel) {
                m_errorLabel->hide();
        if (m_errorStrip)
            m_errorStrip->hide();
                m_errorLabel->clear();
                // The effect must go, or the label keeps an offscreen buffer for the rest of
                // the session. This project has already been caught by that once.
                m_errorLabel->setGraphicsEffect(nullptr);
            }
            if (m_errorDismiss)
                m_errorDismiss->hide();
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    });

    m_errorDismiss = new QPushButton(errRow);
    m_errorDismiss->setIcon(crossIcon(14, theme::kRed()));
    m_errorDismiss->setIconSize(QSize(14, 14));
    // Its own object name rather than "ghost": this button belongs to the error strip, and a
    // transparent button there was too easy to miss. The style below gives it a visible
    // outline and the error colour on hover.
    m_errorDismiss->setObjectName(QStringLiteral("errorDismiss"));
    m_errorDismiss->setCursor(Qt::PointingHandCursor);
    m_errorDismiss->setFixedSize(26, 26);
    m_errorDismiss->setToolTip(QStringLiteral("Dismiss this message"));
    m_errorDismiss->hide();
    connect(m_errorDismiss, &QPushButton::clicked, this, [this] {
        if (m_errorAutoHide)
            m_errorAutoHide->stop();
        m_errorLabel->hide();
        if (m_errorStrip)
            m_errorStrip->hide();
        m_errorDismiss->hide();
        // Also clear the text, so the banner does not reappear with a stale message if
        // something merely toggles its visibility.
        m_errorLabel->clear();
    });
    // Row 0, column 1, aligned to the top of the cell.
    //
    // Aligning to the top is what puts it in the corner: without it the button would be
    // centred against however many lines the message wraps to, which is the behaviour a
    // Windows toast does not have and which looked wrong here for the same reason.
    errGrid->addWidget(m_errorDismiss, 0, 1, Qt::AlignTop | Qt::AlignRight);
    contentLayout->addWidget(errRow);

    shell->addWidget(content, 1);
    setCentralWidget(central);

    m_pages->setCurrentIndex(int(Sidebar::Dashboard));

    statusBar()->showMessage(
        QStringLiteral("Shaping system-wide traffic with WinDivert  \u2022  runs elevated"));
}

void MainWindow::onPageSelected(int page)
{
    if (!m_pages)
        return;
    if (page < 0 || page >= m_pages->count())
        return;

    m_pages->setCurrentIndex(page);

    // A short cross-fade on the incoming page: enough to make the sidebar feel
    // alive without competing with the live graph (which keeps animating at full
    // rate - the tween is on page opacity only, nothing is repainted differently).
    if (QWidget *incoming = m_pages->currentWidget()) {
        auto *fx = new QGraphicsOpacityEffect(incoming);
        fx->setOpacity(0.0);
        // Keep the previous effect from stacking: one effect per page.
        incoming->setGraphicsEffect(fx);

        auto *anim = new QPropertyAnimation(fx, "opacity", incoming);
        anim->setDuration(150);
        anim->setStartValue(0.0);
        anim->setEndValue(1.0);
        anim->setEasingCurve(QEasingCurve::OutCubic);

        // Clean up in a safe order, and only once each.
        //
        // This previously called incoming->setGraphicsEffect(nullptr) from the
        // animation's own finished handler while the animation was ALSO marked
        // DeleteWhenStopped. setGraphicsEffect(nullptr) destroys the effect - the
        // animation's target - and the animation object is then destroyed too, so
        // a page switch could touch freed memory. That is the same class of fault
        // as the Show Less crash (0xc0000005 inside Qt6Core.dll).
        //
        // Now: detach the effect first, then delete the animation, once, from a
        // single place, and clear the effect pointer the page would otherwise keep
        // holding. Detaching is done via deleteLater so any pending repaint that
        // still references the effect has run first.
        connect(anim, &QPropertyAnimation::finished, incoming, [incoming, fx, anim] {
            Q_UNUSED(fx)
            incoming->setGraphicsEffect(nullptr);   // deletes fx, the old target
            anim->deleteLater();                    // now safe: no target in use
        });
        anim->start();
    }

    // Pages read shared state, so refresh the ones whose figures are derived
    // rather than live-bound. Nothing is rebuilt, so live statistics survive.
    switch (page) {
    case Sidebar::Statistics:
        refreshUsage();
        break;
    case Sidebar::Scheduler:
        refreshScheduleIndicators();
        break;
    case Sidebar::Settings:
        if (m_diagnosticsPanel) {
            // Fresh counters the moment the user opens the page.
            if (m_syncDiagnostics)
                m_syncDiagnostics();
            m_diagnosticsPanel->refresh();
        }
        if (m_settingsAdapter)
            m_settingsAdapter->setText(
                QStringLiteral("Current Adapter: %1").arg(m_monitor->activeAdapterName()));
        if (m_settingsDataDir && m_store)
            m_settingsDataDir->setText(m_store->directory());
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

QWidget *MainWindow::buildDashboardPage()
{
    QWidget *body = nullptr;
    QWidget *page = makePage(QStringLiteral("Dashboard"),
                             QStringLiteral("Live network activity and the limits currently in "
                                            "force."),
                             &body);
    auto *layout = qobject_cast<QVBoxLayout *>(body->layout());
    body->setParent(page);

    // The four zones cannot all fit their own minimum heights in a short window
    // - the scheduler card alone needs 236px and was being given 162px, which
    // made Qt overlap the rule list with the helper text. Rather than let the
    // content be squeezed, the dashboard scrolls once it runs out of room. The
    // cards therefore always get at least their minimum height.
    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // ALWAYS ON, not AsNeeded: if the vertical scrollbar appeared when the panel
    // expanded, the viewport would narrow by its width and every card would
    // re-flow horizontally in one frame. That single-frame shift is the "push"
    // reported on Show More. The theme styles it as a slim 10px bar with a
    // transparent track, so it reads as window furniture rather than as chrome.
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    // Kept as a member so the Show More flow can walk the viewport down to the
    // Application Network Activity panel after it expands - without this the panel
    // appeared off-screen and the button read as dead.
    m_dashScroll = scroll;
    auto *canvas = new QWidget();
    auto *canvasLayout = new QVBoxLayout(canvas);
    canvasLayout->setContentsMargins(0, 0, 0, 0);
    canvasLayout->setSpacing(12);

    // The viewport owns the width. ResponsivePair keeps the graph and usage
    // cards at their natural height while the activity panel changes its clip.

    // -- top row: Live Speed | 30-Day Usage --------------------------------
    // Side by side when there is room, stacked when there is not. A plain
    // QHBoxLayout inverted the proportions at the minimum window size: once both
    // cards reach their own minimum widths Qt stops honouring the 62/38 stretch,
    // and the Live Speed card ended up narrower than Usage despite asking for the
    // larger share.
    //
    // The pair is built first and the cards are parented INTO it. Reparenting
    // them afterwards detached each card frame from its own contents, which left
    // the graph and the usage bar laid out against the pair instead of the card.
    auto *topPair = new ResponsivePair(canvas);
    topPair->setPair(buildLiveSpeedCard(topPair), buildUsageCard(topPair), 62, 38);
    topPair->setStackThreshold(940);
    canvasLayout->addWidget(topPair);

    // -- Application Network Activity --------------------------------------
    // A separate full-width card. Wide: directly beneath the top row. Narrow
    // (stacked): embedded BETWEEN Live Speed and Usage, because that is where the
    // eye is when the button on the Live Speed card is pressed - the panel follows
    // the card that opened it instead of appearing detached below the Usage card.
    m_appPanel = new AppActivityPanel(m_appMonitor, canvas);
    m_appPanel->setVisible(false);

    auto placeAppPanel = [this, canvasLayout, topPair](bool stacked) {
        if (!m_appPanel)
            return;
        if (stacked)
            topPair->embedMiddle(m_appPanel);
        else {
            topPair->takeMiddle(m_appPanel);
            canvasLayout->insertWidget(1, m_appPanel);   // right after the row
        }
        m_appPanel->setVisible(m_appPanel->isExpanded() || m_appPanel->isAnimating());
    };

    connect(m_appPanel, &AppActivityPanel::expandedChanged, this, [this](bool expanded) {
        if (m_showMoreButton)
            m_showMoreButton->setText(expanded ? QStringLiteral("Show Less \u02C4")
                                               : QStringLiteral("Show More \u02C4"));
    });

    struct RevealAnchor {
        int savedMinimum = -1;
        int from = 0;
        int to = 0;
        int scrollFrom = 0;
        int scrollTo = 0;
        bool userScrolled = false;
    };
    const auto anchor = std::make_shared<RevealAnchor>();
    // Reserve enough canvas for BOTH endpoints before the first frame. Letting
    // QScrollArea grow it later can lay out a taller panel in a too-short canvas
    // for a frame, squeezing the gaps and visibly moving the card up and down.
    // The trailing stretch takes the unused room during the reveal.
    connect(m_appPanel, &AppActivityPanel::revealStarted, this,
            [this, canvas, canvasLayout, scroll, anchor](int from, int to) {
        auto *bar = scroll->verticalScrollBar();
        if (anchor->savedMinimum < 0)
            anchor->savedMinimum = canvas->minimumHeight();
        anchor->from = from;
        anchor->to = to;
        anchor->scrollFrom = bar->value();
        anchor->userScrolled = false;
        const int finalContent = canvasLayout->minimumSize().height() - from + to;
        const int finalMaximum = qMax(0, finalContent - scroll->viewport()->height());
        anchor->scrollTo = to < from ? qMin(bar->value(), finalMaximum) : bar->value();
        if (to == 0 && m_showMoreButton) {
            const int toggleY = m_showMoreButton->mapTo(canvas, QPoint()).y();
            anchor->scrollTo = qMin(anchor->scrollTo, qMax(0, toggleY - 12));
            // Hiding a focused footer otherwise transfers focus to the next
            // card, making QScrollArea jump down after the reveal has ended.
            // Return focus to the opener now, keeping scrolling on the same
            // animation timeline instead of Qt's immediate ensure-visible.
            if (auto *focus = QApplication::focusWidget();
                focus && m_appPanel->isAncestorOf(focus)) {
                m_showMoreButton->setFocus(Qt::OtherFocusReason);
                bar->setValue(anchor->scrollFrom);
            }
        }
        const int reservedHeight = qMax(qMax(canvas->height(), finalContent), anchor->savedMinimum);
        canvas->setMinimumHeight(reservedHeight);
        canvasLayout->activate();
    });
    connect(m_appPanel, &AppActivityPanel::revealTick, this,
            [scroll, anchor, topPair, canvasLayout](int height) {
        // In the narrow layout the panel is a grandchild of the canvas. Its
        // new height invalidates the inner layout first; waiting for the outer
        // LayoutRequest lets the inner layout paint with the previous height,
        // compressing or spreading its gaps for one frame. Propagate and apply
        // the new size through both layout levels before any repaint.
        if (topPair->isStacked()) {
            topPair->layout()->activate();
            topPair->updateGeometry();
            canvasLayout->activate();
        }
        if (anchor->savedMinimum < 0 || anchor->from == anchor->to || anchor->userScrolled)
            return;
        const qreal progress = qBound(0.0, qreal(height - anchor->from)
                                             / (anchor->to - anchor->from), 1.0);
        scroll->verticalScrollBar()->setValue(qRound(anchor->scrollFrom
            + progress * (anchor->scrollTo - anchor->scrollFrom)));
    });
    connect(scroll->verticalScrollBar(), &QAbstractSlider::actionTriggered, this,
            [anchor](int) { anchor->userScrolled = true; });
    connect(scroll->verticalScrollBar(), &QAbstractSlider::sliderPressed, this,
            [anchor] { anchor->userScrolled = true; });
    connect(m_appPanel, &AppActivityPanel::revealFinished, this, [this, canvas, anchor] {
        if (anchor->savedMinimum >= 0) {
            canvas->setMinimumHeight(anchor->savedMinimum);
            anchor->savedMinimum = -1;
        }
        QTimer::singleShot(0, this, [this] {
            if (!m_appPanel->isAnimating())
                refreshUsage();
        });
    });
    connect(topPair, &ResponsivePair::orientationChanged, this, placeAppPanel);
    // Apply the initial placement: applyLayout may already have decided "stacked"
    // before this panel existed, in which case no orientation signal will come.
    placeAppPanel(topPair->isStacked());

    canvasLayout->addWidget(buildLimiterCard(canvas));
    canvasLayout->addWidget(buildSchedulerCard(canvas));

    canvasLayout->addStretch(1);

    scroll->setWidget(canvas);
    net::ScrollBarAnimator::install(scroll);   // fades the handle in on hover
    layout->addWidget(scroll, 1);

    // Keep the old vertical weighting available for a window tall enough to show
    // everything at once: the canvas distributes extra height in these ratios.
    topPair->setMinimumHeight(300);

    return page;
}

// ---------------------------------------------------------------------------
// Limiter page
// ---------------------------------------------------------------------------

QWidget *MainWindow::buildSchedulerPage()
{
    QWidget *body = nullptr;
    QWidget *page = makePage(QStringLiteral("Time Scheduler"),
                             QStringLiteral("Manage automatic bandwidth limits."),
                             &body);
    auto *layout = qobject_cast<QVBoxLayout *>(body->layout());

    // -- add button, top right ---------------------------------------------
    auto *actions = new QHBoxLayout();
    actions->addStretch(1);
    m_schedAddButton = new QPushButton(QStringLiteral("+ Add Rule"), body);
    m_schedAddButton->setCursor(Qt::PointingHandCursor);
    connect(m_schedAddButton, &QPushButton::clicked, this, &MainWindow::onAddRule);
    actions->addWidget(m_schedAddButton);
    layout->addLayout(actions);

    // -- status line --------------------------------------------------------
    m_schedPageStatus = new QLabel(body);
    m_schedPageStatus->setObjectName(QStringLiteral("sub"));
    m_schedPageStatus->setWordWrap(true);
    layout->addWidget(m_schedPageStatus);

    // -- empty state --------------------------------------------------------
    m_schedEmptyState = new QWidget(body);
    auto *emptyLayout = new QVBoxLayout(m_schedEmptyState);
    emptyLayout->setContentsMargins(0, 24, 0, 24);
    emptyLayout->setSpacing(8);

    auto *emptyTitle = new QLabel(QStringLiteral("No schedules yet"), m_schedEmptyState);
    emptyTitle->setObjectName(QStringLiteral("statValue"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);

    auto *emptyBody = new QLabel(
        QStringLiteral("Create a schedule to automatically control your internet speed at "
                       "specific times."),
        m_schedEmptyState);
    emptyBody->setObjectName(QStringLiteral("sub"));
    emptyBody->setAlignment(Qt::AlignCenter);
    emptyBody->setWordWrap(true);
    emptyLayout->addWidget(emptyBody);

    m_schedEmptyAddButton = new QPushButton(QStringLiteral("+ Create Schedule"), m_schedEmptyState);
    m_schedEmptyAddButton->setCursor(Qt::PointingHandCursor);
    connect(m_schedEmptyAddButton, &QPushButton::clicked, this, &MainWindow::onAddRule);
    emptyLayout->addWidget(m_schedEmptyAddButton, 0, Qt::AlignHCenter);

    layout->addWidget(m_schedEmptyState);

    // -- scrollable card list ----------------------------------------------
    m_schedPageScroll = new QScrollArea(body);
    m_schedPageScroll->setWidgetResizable(true);
    m_schedPageScroll->setFrameShape(QFrame::NoFrame);
    m_schedPageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_schedPageList = new QWidget();
    m_schedPageLayout = new QVBoxLayout(m_schedPageList);
    m_schedPageLayout->setContentsMargins(0, 0, 0, 0);
    m_schedPageLayout->setSpacing(8);
    m_schedPageLayout->addStretch(1);
    m_schedPageScroll->setWidget(m_schedPageList);
    net::ScrollBarAnimator::install(m_schedPageScroll);   // fades the handle in on hover
    layout->addWidget(m_schedPageScroll, 1);

    return page;
}

// ---------------------------------------------------------------------------
// Statistics page
// ---------------------------------------------------------------------------

QWidget *MainWindow::buildAppLimiterPage()
{
    QWidget *body = nullptr;
    QWidget *page = makePage(QStringLiteral("App Limiter"),
                             QStringLiteral("Control download and upload speed for "
                                            "individual applications."),
                             &body);

    auto *layout = qobject_cast<QVBoxLayout *>(body->layout());

    m_appLimiterPageWidget = new AppLimiterPage(m_appLimitEngine, body);
    connect(m_appLimiterPageWidget, &AppLimiterPage::addRequested, this,
            &MainWindow::onAppRuleAdd);
    connect(m_appLimiterPageWidget, &AppLimiterPage::editRequested, this,
            &MainWindow::onAppRuleEdit);
    connect(m_appLimiterPageWidget, &AppLimiterPage::deleteRequested, this,
            &MainWindow::onAppRuleDelete);
    connect(m_appLimiterPageWidget, &AppLimiterPage::enabledChanged, this,
            &MainWindow::onAppRuleEnabledChanged);
    connect(m_appLimiterPageWidget, &AppLimiterPage::scheduleRequested, this,
            &MainWindow::onAppRuleSchedule);

    layout->addWidget(m_appLimiterPageWidget, 1);
    return page;
}

QWidget *MainWindow::buildStatisticsPage()
{
    QWidget *outerBody = nullptr;
    QWidget *page = makePage(QStringLiteral("Statistics"),
                             QStringLiteral("Data usage recorded by Flowbit."),
                             &outerBody);
    auto *outerLayout = qobject_cast<QVBoxLayout *>(outerBody->layout());

    // The page scrolls, like the Dashboard does.
    //
    // makePage() returns a plain body with no scroll area, and each page that needs
    // one adds its own. This page never did, and it holds five statistic cards plus
    // the per-application table - taller than a short window. Without scrolling the
    // cards above took the height they required, the table was handed what was left
    // (a single visible row), and the rest of the list could not be reached at all.
    auto *scroll = new QScrollArea(outerBody);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Reserved rather than appearing on demand: a scrollbar that comes and goes
    // changes the viewport width and would shift the table's columns.
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    QWidget *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    scroll->setWidget(body);
    net::ScrollBarAnimator::install(scroll);   // fades the handle in on hover
    outerLayout->addWidget(scroll, 1);

    const auto makeStat = [&](const QString &caption, QLabel **valueOut) {
        QFrame *card = makeCard(caption, body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());
        auto *value = new QLabel(QStringLiteral("\u2014"), card);
        value->setObjectName(QStringLiteral("statValue"));
        value->setWordWrap(true);
        cl->addWidget(value);
        *valueOut = value;
        return card;
    };

    auto *row1 = new QHBoxLayout();
    row1->setSpacing(12);
    row1->addWidget(makeStat(QStringLiteral("Today"), &m_statToday), 1);
    row1->addWidget(makeStat(QStringLiteral("Last 7 Days"), &m_statWeek), 1);
    row1->addWidget(makeStat(QStringLiteral("Current Cycle"), &m_statCycle), 1);
    layout->addLayout(row1);

    auto *row2 = new QHBoxLayout();
    row2->setSpacing(12);
    row2->addWidget(makeStat(QStringLiteral("Quota"), &m_statQuota), 1);
    row2->addWidget(makeStat(QStringLiteral("Remaining"), &m_statRemaining), 1);
    layout->addLayout(row2);

    {
        QFrame *card = makeCard(QStringLiteral("Download / Upload Split"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());
        m_statDownUp = new QLabel(QStringLiteral("\u2014"), card);
        m_statDownUp->setObjectName(QStringLiteral("statValue"));
        m_statDownUp->setWordWrap(true);
        cl->addWidget(m_statDownUp);
        layout->addWidget(card);
    }

    {
        QFrame *card = makeCard(QStringLiteral("Usage by Application"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());

        auto *hint = new QLabel(card);
        hint->setObjectName(QStringLiteral("sub"));
        hint->setWordWrap(true);
        cl->addWidget(hint);
        m_appUsageHint = hint;

        m_appUsageEmpty = new QLabel(card);
        m_appUsageEmpty->setObjectName(QStringLiteral("sub"));
        m_appUsageEmpty->setWordWrap(true);
        cl->addWidget(m_appUsageEmpty);

        m_appUsageTable = new QTableWidget(card);
        m_appUsageTable->setColumnCount(4);
        m_appUsageTable->setHorizontalHeaderLabels(
            {QStringLiteral("Application"), QStringLiteral("Total"),
             QStringLiteral("Download"), QStringLiteral("Upload")});
        m_appUsageTable->verticalHeader()->setVisible(false);
        m_appUsageTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_appUsageTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_appUsageTable->setShowGrid(false);
        m_appUsageTable->setFocusPolicy(Qt::NoFocus);
        m_appUsageTable->horizontalHeader()->setHighlightSections(false);

        // Header text alignment must match the column's data alignment: the names are
        // left-aligned and the figures right-aligned, and Qt centres headings by
        // default. Left uncorrected, "Application" floats in the middle of a wide
        // column, far from the names it labels.
        if (QTableWidgetItem *h = m_appUsageTable->horizontalHeaderItem(0))
            h->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        for (int c = 1; c < 4; ++c) {
            if (QTableWidgetItem *h = m_appUsageTable->horizontalHeaderItem(c))
                h->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }

        const auto colourHeading = [&](int column, const QColor &accent) {
            if (QTableWidgetItem *h = m_appUsageTable->horizontalHeaderItem(column))
                h->setForeground(accent);
        };
        colourHeading(2, theme::kGraphDownload());
        colourHeading(3, theme::kGraphUpload());

        // Fixed widths for the numbers, measured from the font, so the columns can
        // never demand more than the card has; the name column takes the remainder.
        const QFontMetrics fm(m_appUsageTable->font());
        const QString sample = QStringLiteral("999.99 GB");
        const auto widthFor = [&](const QString &heading) {
            return qMax(fm.horizontalAdvance(heading), fm.horizontalAdvance(sample)) + 16;
        };
        m_appUsageTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int c = 1; c <= 3; ++c) {
            m_appUsageTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Fixed);
            m_appUsageTable->setColumnWidth(c, widthFor(c == 1   ? QStringLiteral("Total")
                                                       : c == 2 ? QStringLiteral("Download")
                                                                : QStringLiteral("Upload")));
        }
        // Space reserved rather than appearing on demand, so the headings cannot
        // shift when the list grows past the visible rows.
        m_appUsageTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        m_appUsageTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // Keep the header track tied to the viewport width. Without this the header
        // can lay itself out against the default section size and drift away from the
        // rows beneath it, which is what a rendered screenshot showed.
        m_appUsageTable->horizontalHeader()->setStretchLastSection(false);
        m_appUsageTable->horizontalHeader()->setMinimumSectionSize(40);
        m_appUsageTable->setIconSize(QSize(20, 20));
        m_appUsageTable->setMinimumHeight(180);
        cl->addWidget(m_appUsageTable);

        // The toggle the show-all flag was written for. Without it the table was
        // silently capped at ten rows with no way to reach the rest.
        auto *moreRow = new QHBoxLayout();
        moreRow->addStretch(1);
        m_appUsageMore = new QPushButton(card);
        m_appUsageMore->setObjectName(QStringLiteral("ghost"));
        m_appUsageMore->setCursor(Qt::PointingHandCursor);
        connect(m_appUsageMore, &QPushButton::clicked, this, [this] {
            m_appUsageShowAll = !m_appUsageShowAll;
            refreshStatistics();
        });
        moreRow->addWidget(m_appUsageMore);
        cl->addLayout(moreRow);

        layout->addWidget(card);
    }

    {
        QFrame *card = makeCard(QStringLiteral("Cycle Controls"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());
        auto *note = new QLabel(
            QStringLiteral("The quota and cycle length are set on the Dashboard. Resetting "
                           "usage here changes no settings, limits or schedules."),
            card);
        note->setObjectName(QStringLiteral("sub"));
        note->setWordWrap(true);
        cl->addWidget(note);

        auto *row = new QHBoxLayout();
        row->setSpacing(10);
        row->addStretch(1);

        auto *resetUsage = new QPushButton(QStringLiteral("Reset usage statistics"), card);
        resetUsage->setObjectName(QStringLiteral("danger"));
        resetUsage->setCursor(Qt::PointingHandCursor);
        resetUsage->setToolTip(QStringLiteral(
            "Deletes all recorded usage and the per-application history."));
        connect(resetUsage, &QPushButton::clicked, this, [this] {
            const auto answer = QMessageBox::question(
                this, QStringLiteral("Reset usage statistics"),
                QStringLiteral("Delete every recorded usage total and the per-application "
                               "history?\n\nYour settings, limits and schedules are kept."),
                QMessageBox::Reset | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Reset)
                return;

            const int failed = m_usage->resetAllUsage();
            refreshUsage();
            refreshStatistics();
            if (failed > 0) {
                QMessageBox::warning(
                    this, QStringLiteral("Reset usage statistics"),
                    QStringLiteral("%1 usage file(s) could not be deleted, so old figures may "
                                   "return after a restart.").arg(failed));
            } else {
                statusBar()->showMessage(QStringLiteral("Usage statistics cleared"), 4000);
            }
        });
        row->addWidget(resetUsage);
        cl->addLayout(row);

        auto *go = new QPushButton(QStringLiteral("Open Usage Controls"), card);
        go->setObjectName(QStringLiteral("ghost"));
        go->setCursor(Qt::PointingHandCursor);
        connect(go, &QPushButton::clicked, this, [this] {
            if (m_sidebar)
                m_sidebar->setCurrentPage(Sidebar::Dashboard);
        });
        cl->addWidget(go, 0, Qt::AlignLeft);
        layout->addWidget(card);
    }

    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Settings page
// ---------------------------------------------------------------------------

QWidget *MainWindow::buildSettingsPage()
{
    QWidget *outerBody = nullptr;
    QWidget *page = makePage(QStringLiteral("Settings"),
                             QStringLiteral("Appearance, network and application options."),
                             &outerBody);
    auto *outerLayout = qobject_cast<QVBoxLayout *>(outerBody->layout());

    // The page scrolls.
    //
    // Measured before this: the diagnostics panel needs 1215px of height and was given
    // 292px, which left 52 of its 53 labels shorter than their own size hint and
    // crushed the grid rows to zero. makePage() returns a plain body with no scroll
    // area, so there was no way to give the panel the room it needs, nor to reach it.
    auto *scroll = new QScrollArea(outerBody);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    QWidget *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    scroll->setWidget(body);
    net::ScrollBarAnimator::install(scroll);   // fades the handle in on hover
    outerLayout->addWidget(scroll, 1);

    // -- appearance ---------------------------------------------------------
    {
        QFrame *card = makeCard(QStringLiteral("Appearance"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());

        auto *row = new QHBoxLayout();
        row->setSpacing(10);
        auto *label = new QLabel(QStringLiteral("Theme"), card);
        row->addWidget(label);
        row->addStretch(1);

        m_pageThemeCombo = new QComboBox(card);
        m_pageThemeCombo->addItem(QStringLiteral("System"), int(Settings::ThemeMode::Auto));
        m_pageThemeCombo->addItem(QStringLiteral("Light"), int(Settings::ThemeMode::Light));
        m_pageThemeCombo->addItem(QStringLiteral("Dark"), int(Settings::ThemeMode::Dark));
        m_pageThemeCombo->setMinimumWidth(120);
        m_pageThemeCombo->setToolTip(QStringLiteral(
            "System follows the Windows light/dark setting."));
        // Mirrors the dashboard selector; both write the same setting.
        connect(m_pageThemeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) {
                    if (m_loading)
                        return;
                    Settings s = m_store->load();
                    s.themeMode = Settings::ThemeMode(m_pageThemeCombo->currentData().toInt());
                    persistSettings(s, QStringLiteral("the theme"));
                    theme::setMode(s.themeMode == Settings::ThemeMode::Light
                                       ? theme::Mode::Light
                                       : (s.themeMode == Settings::ThemeMode::Dark
                                              ? theme::Mode::Dark
                                              : theme::Mode::Auto));
                    // Keep the dashboard selector in step.
                    if (m_themeCombo) {
                        const int idx = m_themeCombo->findData(int(s.themeMode));
                        if (idx >= 0) {
                            QSignalBlocker b(m_themeCombo);
                            m_themeCombo->setCurrentIndex(idx);
                        }
                    }
                    reapplyWidgetStyles();
                    update();
                    if (centralWidget())
                        centralWidget()->update();
                });
        row->addWidget(m_pageThemeCombo);
        cl->addLayout(row);

        auto *note = new QLabel(
            QStringLiteral("Flowbit stays monochrome. Cyan and orange are reserved for the "
                           "download and upload indicators."),
            card);
        note->setObjectName(QStringLiteral("sub"));
        note->setWordWrap(true);
        cl->addWidget(note);

        layout->addWidget(card);
    }

    // -- startup and closing -------------------------------------------------
    //
    // Placed directly after Appearance and before Network, so it is among the settings rather
    // than after the diagnostics report. It was last in the page, which put it 1428px down a
    // 694px viewport - reachable only by scrolling past an entire diagnostic dump, which is
    // why it appeared not to exist.
    {
        QFrame *card = makeCard(QStringLiteral("Startup"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());

        // -- launch at logon -----------------------------------------------
        auto *row = new QHBoxLayout();
        row->setSpacing(10);
        auto *label = new QLabel(QStringLiteral("Launch Flowbit when Windows starts"), card);
        row->addWidget(label);
        row->addStretch(1);

        m_startupCheck = new QCheckBox(card);
        m_startupCheck->setCursor(Qt::PointingHandCursor);
        // The checkbox carries no text - the row's label describes it - so it is given the
        // column width and right-aligned, which puts its indicator on the same edge as the
        // combo and the button below it.
        m_startupCheck->setFixedWidth(280);
        m_startupCheck->setStyleSheet(QStringLiteral("QCheckBox { padding-left: %1px; }")
                                          .arg(280 - 18 - 2));
        m_startupCheck->setToolTip(QStringLiteral(
            "Starts Flowbit when you log in, minimised to the notification area, so limits "
            "apply without opening a window."));
        connect(m_startupCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_loading || !m_startupCheck)
                return;
            Settings s = m_store->load();
            s.startWithWindows = on;
            QString error;
            persistSettings(s, QStringLiteral("your settings"));

            // The setting and the mechanism are changed together. They are the same fact
            // stored twice - the file records the intent, and a scheduled task or the Run key
            // is what Windows acts on - and letting them disagree is how a toggle ends up
            // looking on while nothing starts.
            applyStartupSetting();
        });
        row->addWidget(m_startupCheck);
        cl->addLayout(row);

        // A short line under it, always visible rather than only on a problem, so the
        // consequence of the choice is clear before it is made.
        auto *startupHint = new QLabel(
            QStringLiteral("Starts minimised, without the administrator prompt. Off means "
                           "Flowbit only runs when you open it yourself."),
            card);
        startupHint->setObjectName(QStringLiteral("hint"));
        startupHint->setWordWrap(true);
        cl->addWidget(startupHint);

        m_startupNote = new QLabel(card);
        m_startupNote->setObjectName(QStringLiteral("hint"));
        m_startupNote->setWordWrap(true);
        m_startupNote->hide();
        cl->addWidget(m_startupNote);

        // -- keep running after the window is closed ------------------------
        //
        // Worded in terms of what the user cares about - whether Flowbit keeps working in the
        // background - rather than in terms of the X button, which is the mechanism and not
        // the point. "When I close the window" told the user what the control governed but not
        // what either choice would do to their limits.
        auto *closeRow = new QHBoxLayout();
        closeRow->setSpacing(10);
        auto *closeLabel = new QLabel(
            QStringLiteral("Keep running in the background after closing the window"), card);
        closeRow->addWidget(closeLabel);
        closeRow->addStretch(1);

        m_closeCombo = new QComboBox(card);
        // The order matters: index 0 is the default, and it is the behaviour Flowbit has
        // always had, so an upgrade does not change what the X button does.
        m_closeCombo->addItem(QStringLiteral("Yes - keep limiting in the background"), 1);
        m_closeCombo->addItem(QStringLiteral("No - quit Flowbit completely"), 0);
        // Fixed to the column width rather than a minimum, so it cannot stretch wider than
        // the checkboxes beside it and break the right edge the rows share.
        m_closeCombo->setFixedWidth(280);
        m_closeCombo->setToolTip(QStringLiteral(
            "Yes hides Flowbit in the notification area, so schedules and limits keep working "
            "after the window is closed. No ends the process and stops every limit."));
        connect(m_closeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) {
                    if (m_loading || !m_closeCombo)
                        return;
                    Settings s = m_store->load();
                    s.closeToTray = m_closeCombo->currentData().toInt() != 0;
                    persistSettings(s, QStringLiteral("the theme"));
                    refreshStartupNote();
                });
        closeRow->addWidget(m_closeCombo);
        cl->addLayout(closeRow);

        // The consequence, stated plainly, because "keep limiting" and "stops every limit" are
        // the two outcomes and neither is obvious from the labels alone when a schedule is
        // waiting to start.
        auto *closeHint = new QLabel(
            QStringLiteral("Keeping it running means a schedule that begins later still "
                           "applies. Quitting releases the driver and stops all limits until "
                           "Flowbit is opened again."),
            card);
        closeHint->setObjectName(QStringLiteral("hint"));
        closeHint->setWordWrap(true);
        cl->addWidget(closeHint);

        // -- updates --------------------------------------------------------
        auto *updateRow = new QHBoxLayout();
        updateRow->setSpacing(10);
        auto *updateLabel = new QLabel(QStringLiteral("Check for updates automatically"), card);
        updateRow->addWidget(updateLabel);
        updateRow->addStretch(1);

        m_updateCheck = new QCheckBox(card);
        m_updateCheck->setCursor(Qt::PointingHandCursor);
        m_updateCheck->setFixedWidth(280);
        m_updateCheck->setStyleSheet(QStringLiteral("QCheckBox { padding-left: %1px; }")
                                         .arg(280 - 18 - 2));
        m_updateCheck->setToolTip(QStringLiteral(
            "Looks for a newer version when Flowbit starts. Nothing is downloaded unless "
            "you accept it."));
        connect(m_updateCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_loading)
                return;
            Settings s = m_store->load();
            s.checkForUpdates = on;
            QString error;
            persistSettings(s, QStringLiteral("your settings"));
        });
        updateRow->addWidget(m_updateCheck);
        cl->addLayout(updateRow);

        // The status line and the button are on separate rows.
        //
        // Sharing a row with the button meant the status text - which wraps - drove the row's
        // height, so a long failure pushed the button down and left the card's rhythm broken.
        // A message that changes length should not move a control the user is about to press.
        m_updateStatus = new QLabel(card);
        // An error, not a hint: this label reports that the check failed, which the user is
        // being asked to act on rather than merely informed of. The style is switched to "hint"
        // when the message is ordinary, in setUpdateStatus.
        m_updateStatus->setObjectName(QStringLiteral("error"));
        m_updateStatus->setWordWrap(true);
        m_updateStatus->hide();   // nothing to report until a check has run
        cl->addWidget(m_updateStatus);

        auto *nowRow = new QHBoxLayout();
        nowRow->setSpacing(10);
        nowRow->addStretch(1);

        m_checkNowButton = new QPushButton(QStringLiteral("Check now"), card);
        m_checkNowButton->setObjectName(QStringLiteral("ghost"));
        m_checkNowButton->setCursor(Qt::PointingHandCursor);
        connect(m_checkNowButton, &QPushButton::clicked, this,
                [this] { checkForUpdatesNow(true); });
        nowRow->addWidget(m_checkNowButton);
        cl->addLayout(nowRow);

        // Added to the page. Every other card does this; omitting it left the Startup card
        // constructed but unplaced, so it was given a default size instead of the height of
        // its contents.
        layout->addWidget(card);
    }

    // -- network ------------------------------------------------------------
    {
        QFrame *card = makeCard(QStringLiteral("Network"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());

        m_settingsAdapter = new QLabel(card);
        m_settingsAdapter->setObjectName(QStringLiteral("statValue"));
        m_settingsAdapter->setWordWrap(true);
        cl->addWidget(m_settingsAdapter);

        auto *note = new QLabel(
            QStringLiteral("Flowbit throttles system-wide traffic, so it follows whichever "
                           "adapter the system is using for internet access and does not "
                           "require you to pick one."),
            card);
        note->setObjectName(QStringLiteral("sub"));
        note->setWordWrap(true);
        cl->addWidget(note);

        layout->addWidget(card);
    }

    // -- application --------------------------------------------------------
    {
        QFrame *card = makeCard(QStringLiteral("Application"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());

        auto *startNote = new QLabel(
            QStringLiteral("Flowbit needs Administrator rights to shape traffic, so it is "
                           "started manually rather than at sign-in. Closing the window keeps "
                           "it running in the notification area."),
            card);
        startNote->setObjectName(QStringLiteral("sub"));
        startNote->setWordWrap(true);
        cl->addWidget(startNote);

        m_settingsDataDir = new QLabel(card);
        m_settingsDataDir->setObjectName(QStringLiteral("sub"));
        m_settingsDataDir->setWordWrap(true);
        m_settingsDataDir->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cl->addWidget(m_settingsDataDir);

        layout->addWidget(card);
    }

    layout->addStretch(1);
        // -- diagnostics --------------------------------------------------------
    // A complete, readable picture of why limiting may not be working. It reads
    // state only and changes nothing, so it is always safe to open, and it is
    // exportable so a report can be shared without paraphrasing it.
    {
        QFrame *card = makeCard(QStringLiteral("Diagnostics"), body);
        auto *dl = qobject_cast<QVBoxLayout *>(card->layout());

        m_diagnosticsPanel = new DiagnosticsPanel(card);
        // Carry the application monitor's failure reason into the report, and keep
        // it current so the report and the activity panel tell one story.
        m_diagnosticsPanel->setMonitorError(m_appMonitor ? m_appMonitor->errorString()
                                                         : QString());
        auto syncDiagnostics = [this] {
            if (!m_diagnosticsPanel)
                return;
            m_diagnosticsPanel->setMonitorError(m_appMonitor->errorString());
            m_diagnosticsPanel->setMonitorRunning(m_appMonitor->isRunning());
            m_diagnosticsPanel->setMonitorTelemetry(m_appMonitor->eventsSeenTotal(),
                                                    m_appMonitor->eventsDecodedTotal(),
                                                    m_appMonitor->udpEventsTotal(),
                                                    m_appMonitor->runningSeconds());
            m_diagnosticsPanel->setEventLayout(m_appMonitor->etwEventLayout());
            m_diagnosticsPanel->setAddressMiss(m_appMonitor->addressMissReport());
            m_diagnosticsPanel->setRawEvents(m_appMonitor->rawEventReport());
            m_diagnosticsPanel->setRawBits(m_appMonitor->rawBitsReport());
            m_diagnosticsPanel->setEventIds(m_appMonitor->eventIdReport());
            m_diagnosticsPanel->setConsumerState(m_appMonitor->consumerReport());
            if (m_diagnosticsPanel->isVisible())
                m_diagnosticsPanel->refresh();
        };
        connect(m_appMonitor, &AppMonitor::stateChanged, this, syncDiagnostics);
        m_syncDiagnostics = syncDiagnostics;
        connect(m_diagnosticsPanel, &DiagnosticsPanel::exported, this,
                [this](bool ok, const QString &message) {
                    // A success goes to the status bar, not the error strip.
                    //
                    // The strip is an alert: a dark card with a red accent, sized for a sentence
                    // about the driver, and it stays until dismissed. Pushing "Report copied to
                    // the clipboard" through it produced a red-bordered box announcing a
                    // success, permanently on screen, grown to fit a message longer than the
                    // strip was laid out for. All three were reported.
                    //
                    // The status bar already carries messages of this kind - the reset uses it -
                    // and clears itself after a few seconds. Nothing needs acknowledging, so
                    // nothing needs a card.
                    if (ok) {
                        statusBar()->showMessage(message, 4000);
                        return;
                    }

                    // A failure is a fault, and faults belong in the strip - with the same
                    // auto-clear every other fault gets. Not starting the timer here is what
                    // left the message on screen until it was dismissed by hand.
                    if (!m_errorLabel)
                        return;
                    m_errorLabel->setText(message);
                    m_errorLabel->setStyleSheet(QString());
                    showErrorStrip();
                });
        dl->addWidget(m_diagnosticsPanel);

        auto *hint = new QLabel(
            QStringLiteral("This report only reads system state. It changes nothing, and "
                           "contains no personal data - no addresses, no filenames you "
                           "have not chosen, and no traffic contents."),
            card);
        hint->setObjectName(QStringLiteral("sub"));
        hint->setWordWrap(true);
        dl->addWidget(hint);

        layout->addWidget(card);
    }



    // -- reset ---------------------------------------------------------------
    //
    // Last on the page, below the diagnostics report. A destructive action should be met after
    // everything else, not passed on the way to the theme selector.
    {
        QFrame *card = makeCard(QStringLiteral("Reset"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());

        auto *exclaim = new QLabel(
            QStringLiteral("Returns Flowbit to a fresh install."), card);
        cl->addWidget(exclaim);

        auto *what = new QLabel(
            QStringLiteral("Every setting is cleared: the system-wide limits, all schedules, "
                           "every application rule, the appearance and the startup behaviour. "
                           "The 30-day usage figures and the per-application breakdown are "
                           "deleted as well, and those are measured data - they cannot be "
                           "recovered."),
            card);
        what->setObjectName(QStringLiteral("hint"));
        what->setWordWrap(true);
        cl->addWidget(what);

        auto *row = new QHBoxLayout();
        row->setSpacing(10);
        row->addStretch(1);

        m_resetButton = new QPushButton(QStringLiteral("Reset everything"), card);
        m_resetButton->setObjectName(QStringLiteral("danger"));
        m_resetButton->setCursor(Qt::PointingHandCursor);
        m_resetButton->setToolTip(QStringLiteral(
            "Clears all settings, schedules, application rules and usage history."));
        connect(m_resetButton, &QPushButton::clicked, this, &MainWindow::resetEverything);
        row->addWidget(m_resetButton);
        cl->addLayout(row);

        layout->addWidget(card);
    }

    // -- uninstall -----------------------------------------------------------
    //
    // Below reset, which is the order of severity: resetting keeps the application, this removes
    // it. Both are at the end of the page, past everything a user changes in normal use.
    {
        QFrame *card = makeCard(QStringLiteral("Uninstall"), body);
        auto *cl = qobject_cast<QVBoxLayout *>(card->layout());

        auto *lead = new QLabel(QStringLiteral("Remove Flowbit from this computer."), card);
        cl->addWidget(lead);

        auto *what = new QLabel(
            QStringLiteral("Closes Flowbit, removes the program and its shortcuts, and "
                           "deregisters the network driver. You are asked whether to keep your "
                           "settings and usage history as part of the removal."),
            card);
        what->setObjectName(QStringLiteral("hint"));
        what->setWordWrap(true);
        cl->addWidget(what);

        m_uninstallNote = new QLabel(card);
        m_uninstallNote->setObjectName(QStringLiteral("hint"));
        m_uninstallNote->setWordWrap(true);
        m_uninstallNote->hide();
        cl->addWidget(m_uninstallNote);

        auto *row = new QHBoxLayout();
        row->setSpacing(10);
        row->addStretch(1);

        m_uninstallButton = new QPushButton(QStringLiteral("Uninstall Flowbit"), card);
        m_uninstallButton->setObjectName(QStringLiteral("danger"));
        m_uninstallButton->setCursor(Qt::PointingHandCursor);
        m_uninstallButton->setToolTip(QStringLiteral(
            "Runs the uninstaller. You are asked whether to keep your settings."));
        connect(m_uninstallButton, &QPushButton::clicked, this, &MainWindow::uninstallFlowbit);
        row->addWidget(m_uninstallButton);
        cl->addLayout(row);

        layout->addWidget(card);
    }

    return page;


}

QWidget *MainWindow::buildLiveSpeedCard(QWidget *parent)
{
    // Parented to the caller when one is given, so a responsive container can
    // own the card outright rather than having it reparented later.
    QFrame *card = makeCard(QStringLiteral("Live Speed"), parent ? parent : this);
    auto *layout = qobject_cast<QVBoxLayout *>(card->layout());
    // Wider bottom inset than the standard card: the graph's axis label sat
    // against the frame edge with the default 13px.
    layout->setContentsMargins(12, 12, 12, 14);

    auto *counterRow = new QHBoxLayout();
    counterRow->setSpacing(18);
    // Breathing room under the readouts, so the graph never butts against them.
    counterRow->setContentsMargins(0, 0, 0, 6);

    auto makeCounter = [&](const QString &arrow, const QString &caption, const QColor &colour,
                           QLabel **out, QLabel **captionOut) {
        auto *col = new QVBoxLayout();
        col->setSpacing(0);

        auto *head = new QLabel(QStringLiteral("%1 %2").arg(arrow, caption), card);
        head->setObjectName(QStringLiteral("sub"));

        auto *value = new QLabel(QStringLiteral("0 B/s"), card);
        value->setFont(theme::counterFont(16, true));
        value->setStyleSheet(QStringLiteral("color: %1;").arg(colour.name()));

        col->addWidget(head);
        col->addWidget(value);
        counterRow->addLayout(col);
        *out = value;
        if (captionOut)
            *captionOut = head;
    };

    // Direction is carried by the arrow glyph and by text value (primary ink for
    // download, secondary for upload) rather than by hue, which keeps the chrome
    // strictly monochrome.
    makeCounter(QStringLiteral("\u2193"), QStringLiteral("DOWNLOAD"), theme::kTextPrimary(),
                &m_downCounter, &m_downCounterCaption);
    makeCounter(QStringLiteral("\u2191"), QStringLiteral("UPLOAD"), theme::kTextSecondary(),
                &m_upCounter, &m_upCounterCaption);
    counterRow->addStretch(1);
    layout->addLayout(counterRow);

    m_graph = new SpeedGraph(card);
    // A hard floor: letting the layout squeeze this was the "squish" complaint.
    m_graph->setMinimumHeight(100);
    m_graph->setToolTip(QStringLiteral(
        "Throughput over the last 60 seconds.\nThe dashed line marks the active download cap."));
    layout->addWidget(m_graph, 1);

    // -- Show More ---------------------------------------------------------
    // Reveals the Application Network Activity panel, which is a separate
    // full-width card beneath the top row. Growing this card alone would make the
    // row lopsided, because 30-Day Usage sits beside it.
    layout->addSpacing(4);
    auto *moreRow = new QHBoxLayout();
    moreRow->addStretch(1);

    m_showMoreButton = new QPushButton(QStringLiteral("Show More \u02C4"), card);
    m_showMoreButton->setObjectName(QStringLiteral("ghost"));
    m_showMoreButton->setCursor(Qt::PointingHandCursor);
    m_showMoreButton->setToolTip(
        QStringLiteral("Show which applications are using your connection"));
    connect(m_showMoreButton, &QPushButton::clicked, this, [this] {
        const bool expand = m_showMoreButton->text().startsWith(QStringLiteral("Show More"));
        if (m_appPanel)
            m_appPanel->setExpanded(expand);
    });
    moreRow->addWidget(m_showMoreButton);
    layout->addLayout(moreRow);

    return card;
}

QWidget *MainWindow::buildUsageCard(QWidget *parent)
{
    QFrame *card = makeCard(QStringLiteral("30-Day Data Usage"), parent ? parent : this);
    auto *layout = qobject_cast<QVBoxLayout *>(card->layout());

    m_usageReading = new QLabel(QStringLiteral("0 GB / 500 GB"), card);
    m_usageReading->setFont(theme::counterFont(14, true));
    layout->addWidget(m_usageReading);

    m_usageBar = new SegmentedBar(card);
    layout->addWidget(m_usageBar);

    m_usageReset = new QLabel(QStringLiteral("Resets in: --"), card);
    m_usageReset->setObjectName(QStringLiteral("sub"));
    layout->addWidget(m_usageReset);

    // Quota editor: an explicit horizontal layout so the label and the spin box
    // are laid out rather than concatenated into one string.
    auto *quotaRow = new QHBoxLayout();
    quotaRow->setSpacing(8);
    auto *quotaLabel = new QLabel(QStringLiteral("Quota"), card);
    quotaLabel->setObjectName(QStringLiteral("sub"));
    m_quotaSpin = new QDoubleSpinBox(card);
    m_quotaSpin->setRange(1.0, 100000.0);
    m_quotaSpin->setDecimals(0);
    m_quotaSpin->setSuffix(QStringLiteral(" GB"));
    m_quotaSpin->setValue(500);
    m_quotaSpin->setMinimumWidth(96);
    m_quotaSpin->setMaximumWidth(120);
    m_quotaSpin->setToolTip(QStringLiteral("Data allowance used for the reading above"));
    connect(m_quotaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) {
                if (!m_loading)
                    m_applyDebounce->start();
            });
    quotaRow->addWidget(quotaLabel);
    quotaRow->addWidget(m_quotaSpin);
    quotaRow->addStretch(1);
    layout->addLayout(quotaRow);

    auto *splitRow = new QHBoxLayout();
    splitRow->setSpacing(24);
    splitRow->setContentsMargins(0, 10, 0, 0);
    const auto addSplit = [&](const QString &caption, const QString &name, QLabel **value) {
        auto *column = new QVBoxLayout();
        column->setSpacing(2);
        auto *label = new QLabel(caption, card);
        label->setObjectName(QStringLiteral("sub"));
        *value = new QLabel(QStringLiteral("--"), card);
        (*value)->setObjectName(name);
        (*value)->setFont(theme::counterFont(11, true));
        column->addWidget(label);
        column->addWidget(*value);
        splitRow->addLayout(column);
    };
    addSplit(QStringLiteral("Downloaded"), QStringLiteral("usageDownloaded"), &m_usageSplitDown);
    addSplit(QStringLiteral("Uploaded"), QStringLiteral("usageUploaded"), &m_usageSplitUp);
    splitRow->addStretch(1);
    layout->addLayout(splitRow);

    m_usageProjection = new QLabel(QStringLiteral("At this rate: --"), card);
    m_usageProjection->setObjectName(QStringLiteral("usageProjection"));
    m_usageProjection->setWordWrap(true);
    layout->addWidget(m_usageProjection);

    layout->addStretch(1);

    // Action tray: its own row, with real margins and a floor on button width so
    // the labels cannot be clipped.
    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->setContentsMargins(0, 6, 0, 0);

    auto *cycleButton = new QPushButton(QStringLiteral("Reset Cycle"), card);
    cycleButton->setObjectName(QStringLiteral("ghost"));
    cycleButton->setMinimumWidth(90);
    cycleButton->setMinimumHeight(26);
    cycleButton->setToolTip(
        QStringLiteral("Start a fresh quota cycle from today without erasing history"));
    connect(cycleButton, &QPushButton::clicked, this, &MainWindow::onResetCycle);

    auto *nowButton = new QPushButton(QStringLiteral("Reset Now"), card);
    nowButton->setObjectName(QStringLiteral("danger"));
    nowButton->setMinimumWidth(90);
    nowButton->setMinimumHeight(26);
    nowButton->setToolTip(QStringLiteral("Zero the counters for the current cycle"));
    connect(nowButton, &QPushButton::clicked, this, &MainWindow::onResetNow);

    actions->addWidget(cycleButton);
    actions->addWidget(nowButton);
    actions->addStretch(1);
    layout->addLayout(actions);

    return card;
}

QWidget *MainWindow::buildLimiterCard(QWidget *parent)
{
    QFrame *card = makeCard(QString(), parent ? parent : this);
    auto *layout = qobject_cast<QVBoxLayout *>(card->layout());

    auto *header = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("ACTIVE SPEED LIMITER"), card);
    title->setObjectName(QStringLiteral("cardTitle"));
    header->addWidget(title);
    header->addStretch(1);

    // Appearance control, beside the master switch. "Auto" follows the Windows
    // apps-light/dark setting; the other two force a theme.
    auto *themeLabel = new QLabel(QStringLiteral("Theme"), card);
    themeLabel->setObjectName(QStringLiteral("sub"));
    header->addWidget(themeLabel);

    m_themeCombo = new QComboBox(card);
    m_themeCombo->addItem(QStringLiteral("Auto"), int(Settings::ThemeMode::Auto));
    m_themeCombo->addItem(QStringLiteral("Light"), int(Settings::ThemeMode::Light));
    m_themeCombo->addItem(QStringLiteral("Dark"), int(Settings::ThemeMode::Dark));
    m_themeCombo->setFixedWidth(96);
    m_themeCombo->setToolTip(QStringLiteral(
        "Auto follows the Windows light/dark setting.\nLight and Dark force a theme."));
    connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onThemeModeChanged);
    header->addWidget(m_themeCombo);

    header->addSpacing(10);

    m_masterSwitch = new ToggleSwitch(card);
    connect(m_masterSwitch, &ToggleSwitch::toggled, this, &MainWindow::onMasterToggled);
    header->addWidget(m_masterSwitch);
    layout->addLayout(header);

    auto makeDirectionRow = [&](const QString &label, const QColor &colour,
                                TrackSlider **sliderOut, QDoubleSpinBox **valueOut,
                                QComboBox **unitOut, QLabel **captionOut) -> QHBoxLayout * {
        auto *row = new QHBoxLayout();
        row->setSpacing(10);

        auto *caption = new QLabel(label, card);
        caption->setMinimumWidth(84);
        caption->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(colour.name()));
        if (captionOut)
            *captionOut = caption;

        auto *slider = new TrackSlider(colour, card);
        slider->setRange(0, kSliderSteps);
        slider->setMinimumWidth(180);

        auto *value = new QDoubleSpinBox(card);
        value->setRange(0.0, 100000.0);
        value->setDecimals(2);
        value->setSingleStep(0.25);
        value->setMinimumWidth(96);
        value->setMaximumWidth(104);
        // Explicitly editable: the field is the precise-entry path and must
        // accept typed input, keep focus, and commit on Enter or focus-out
        // rather than on every keystroke (keyboard tracking off).
        value->setReadOnly(false);
        value->setFocusPolicy(Qt::StrongFocus);
        value->setKeyboardTracking(false);
        value->setAccelerated(true);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setToolTip(QStringLiteral(
            "Exact limit - type a value and press Enter. 0 means uncapped."));

        auto *unit = new QComboBox(card);
        for (SpeedUnit u : allSpeedUnits())
            unit->addItem(speedUnitLabel(u), int(u));
        unit->setMaximumWidth(84);
        unit->setToolTip(QStringLiteral("Unit for the value to the left"));

        row->addWidget(caption);
        row->addWidget(slider, 1);
        row->addWidget(value);
        row->addWidget(unit);

        *sliderOut = slider;
        *valueOut = value;
        *unitOut = unit;
        return row;
    };

    layout->addLayout(makeDirectionRow(QStringLiteral("Download"), theme::kTextPrimary(), &m_downSlider,
                                       &m_downValue, &m_downUnit, &m_downSliderCaption));
    layout->addLayout(makeDirectionRow(QStringLiteral("Upload"), theme::kTextSecondary(), &m_upSlider,
                                       &m_upValue, &m_upUnit, &m_upSliderCaption));

    connect(m_downSlider, &TrackSlider::valueChanged, this, &MainWindow::onDownloadSlider);
    connect(m_upSlider, &TrackSlider::valueChanged, this, &MainWindow::onUploadSlider);
    connect(m_downValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &MainWindow::onNumericalEdited);
    connect(m_upValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &MainWindow::onNumericalEdited);
    connect(m_downUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onUnitChanged);
    connect(m_upUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onUnitChanged);

    // -- presets -----------------------------------------------------------
    auto *presetRow = new QHBoxLayout();
    presetRow->setSpacing(8);
    auto *presetLabel = new QLabel(QStringLiteral("Presets"), card);
    presetLabel->setObjectName(QStringLiteral("sub"));
    presetRow->addWidget(presetLabel);

    for (const Preset &preset : kPresets) {
        auto *button = new QPushButton(QString::fromLatin1(preset.label), card);
        button->setObjectName(QStringLiteral("preset"));
        button->setToolTip(QStringLiteral("Snap limits to %1 MB/s down / %2 KB/s up")
                               .arg(preset.downBytes / (1024.0 * 1024.0), 0, 'f', 1)
                               .arg(preset.upBytes / 1024.0, 0, 'f', 0));
        connect(button, &QPushButton::clicked, this, &MainWindow::onPresetClicked);
        presetRow->addWidget(button);
    }

    auto *custom = new QPushButton(QStringLiteral("+ Add Custom"), card);
    custom->setObjectName(QStringLiteral("preset"));
    custom->setToolTip(QStringLiteral("Save the current limits as a preset (not implemented yet)"));
    custom->setEnabled(false);
    presetRow->addWidget(custom);

    presetRow->addStretch(1);
    layout->addLayout(presetRow);

    return card;
}

QWidget *MainWindow::buildSchedulerCard(QWidget *parent)
{
    QFrame *card = makeCard(QString(), parent ? parent : this);
    auto *layout = qobject_cast<QVBoxLayout *>(card->layout());

    // -- header ------------------------------------------------------------
    // Only Add remains. The old Cancel and Remove buttons are gone: each card
    // carries its own Edit and Delete, so there is no "selected rule" for them to
    // act on and no way for them to act on the wrong one.
    auto *header = new QHBoxLayout();
    header->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("TIME SCHEDULER"), card);
    title->setObjectName(QStringLiteral("cardTitle"));
    header->addWidget(title);
    header->addStretch(1);

    auto *addRule = new QPushButton(QStringLiteral("+ Add Rule"), card);
    addRule->setCursor(Qt::PointingHandCursor);
    addRule->setToolTip(QStringLiteral("Create a new schedule"));
    connect(addRule, &QPushButton::clicked, this, &MainWindow::onAddRule);
    header->addWidget(addRule);

    layout->addLayout(header);

    // -- scrollable rule list ----------------------------------------------
    // Tall enough for a two-line row plus part of the next, so the list reads as
    // scrollable rather than as one cramped slot.
    m_ruleScroll = new QScrollArea(card);
    // Named so tests and styling can target the dashboard list specifically;
    // findChild<QScrollArea*>() also matches the Scheduler page's list.
    m_ruleScroll->setObjectName(QStringLiteral("ruleListScroll"));
    m_ruleScroll->setWidgetResizable(true);
    m_ruleScroll->setFrameShape(QFrame::NoFrame);
    m_ruleScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Sized so TWO single-line rows are fully visible without scrolling:
    // 2 x 38px rows + 4px spacing + 16px list padding = 96px, plus 2px for the
    // frame. Previously 84px against 74px-tall reflowed rows, so only one row and
    // a sliver fitted - which is what made the strip feel cramped.
    m_ruleScroll->setMinimumHeight(100);
    m_ruleScroll->setToolTip(QStringLiteral("Scheduled windows, evaluated top to bottom"));
    // Inset the list inside its own sunken frame so a rule card sits inside the
    // box with even padding instead of touching the border. Kept neutral: the
    // schedule surface carries no accent colour.
    m_ruleScroll->setStyleSheet(
        QStringLiteral("QScrollArea { background-color: %1; border: 1px solid %2; "
                       "border-radius: 8px; }")
            .arg(theme::kPanelSunken().name(), theme::kBorder().name()));
    m_ruleScroll->viewport()->setAutoFillBackground(false);

    m_ruleList = new QWidget();
    m_ruleList->setObjectName(QStringLiteral("ruleList"));
    m_ruleLayout = new QVBoxLayout(m_ruleList);
    // 8px inner padding on all sides, so a rule card never touches the frame.
    m_ruleLayout->setContentsMargins(8, 8, 8, 8);
    m_ruleLayout->setSpacing(4);
    m_ruleLayout->addStretch(1);   // keeps rows pinned to the top
    m_ruleScroll->setWidget(m_ruleList);
    net::ScrollBarAnimator::install(m_ruleScroll);   // fades the handle in on hover

    layout->addWidget(m_ruleScroll, 1);

    // A real gap in the card's layout. Contents margins on the label only pad
    // inside its own rect, so the label's top edge still met the scroll area's
    // bottom edge - a 1px overlap once the rows grew taller.
    layout->addSpacing(6);

    // -- static helper text, deliberately outside the scroll area ----------
    auto *hint = new QLabel(
        QStringLiteral("Times accept \"22:00\" or \"2200\". Windows may wrap past midnight; "
                       "0 means uncapped."),
        card);
    hint->setObjectName(QStringLiteral("hint"));
    hint->setWordWrap(true);
    hint->setContentsMargins(2, 0, 2, 0);
    // The hint and the list must never overlap. Because the page layout is free
    // to compress this card, the hint is pinned: it cannot shrink below its text,
    // and the list absorbs the remaining space.
    hint->setMinimumHeight(hint->sizeHint().height());
    hint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(hint);
    layout->addSpacing(2);

    // -- status footer -----------------------------------------------------
    auto *footer = new QHBoxLayout();
    m_footerStatus = new QLabel(QStringLiteral("Status: --"), card);
    m_footerStatus->setObjectName(QStringLiteral("sub"));
    m_footerAdapter = new QLabel(card);
    m_footerAdapter->setObjectName(QStringLiteral("sub"));
    footer->addWidget(m_footerStatus, 1);
    footer->addWidget(m_footerAdapter);
    layout->addLayout(footer);

    return card;
}

void MainWindow::buildTray()
{
    // The updater lives as long as the window. It is created here rather than inside the
    // settings page so a check can also be started at launch.
    m_updater = new Updater(this);

    // Connected once, not per check: a second check used to stack another pair of
    // handlers, so a deferral restarted the snooze timer several times.
    connect(m_updater, &Updater::updateSkipped, this, [this](const QString &version) {
        Settings s = m_store->load();
        s.skippedVersion = version;
        persistSettings(s, QStringLiteral("your settings"));
        setUpdateStatus(QStringLiteral("Version %1 skipped. A newer version will be offered.")
                            .arg(version),
                        false);
    });
    connect(m_updater, &Updater::updateDeferred, this, [this](const QString &version) {
        setUpdateStatus(QStringLiteral("Update %1 postponed. It will be offered again.")
                            .arg(version),
                        false);

        if (!m_updateSnooze) {
            m_updateSnooze = new QTimer(this);
            m_updateSnooze->setSingleShot(true);
            m_updateSnooze->setInterval(kUpdateSnoozeMs);
            connect(m_updateSnooze, &QTimer::timeout, this, [this] { checkForUpdatesNow(false); });
        }
        m_updateSnooze->start();
    });

    m_tray = new QSystemTrayIcon(this);
    updateTrayIcon(false);

    auto *menu = new QMenu(this);

    m_trayEnabledAction = new QAction(QStringLiteral("Enable speed limiting"), this);
    m_trayEnabledAction->setCheckable(true);
    connect(m_trayEnabledAction, &QAction::toggled, this, &MainWindow::toggleFromTray);
    menu->addAction(m_trayEnabledAction);

    menu->addSeparator();

    auto *showAction = new QAction(QStringLiteral("Show Flowbit"), this);
    connect(showAction, &QAction::triggered, this, &MainWindow::showAndRaise);
    menu->addAction(showAction);

    auto *aboutAction = new QAction(QStringLiteral("About Flowbit"), this);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
    menu->addAction(aboutAction);

    menu->addSeparator();

    auto *quitAction = new QAction(QStringLiteral("Quit"), this);
    connect(quitAction, &QAction::triggered, this, [this] {
        // Hide before the destructors run their synchronous teardown, or the window stays
        // visible and frozen for as long as that takes.
        hide();
        QApplication::quit();
    });
    menu->addAction(quitAction);

    m_tray->setContextMenu(menu);
    m_tray->setToolTip(QStringLiteral("Flowbit"));
    connect(m_tray, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    m_tray->show();
}

QIcon MainWindow::makeTrayIcon(bool throttling) const
{
    // The mark comes from the vector source, not from painted primitives.
    //
    // The previous version built a conventional letter F here (a rounded
    // rectangular stem plus two bars). That was an invented shape: the real
    // Flowbit logo is two flowing ribbon paths. Drawing from the SVG keeps this
    // glyph, the packaged .ico and the exported PNGs identical by construction.
    //
    // The plate follows the active theme, so the icon sits correctly on a light
    // or dark taskbar.
    return brand::mark(256, /*slash=*/!throttling, /*plateForDarkTheme=*/theme::isDark());
}

void MainWindow::updateTrayIcon(bool throttling)
{
    if (!m_tray)
        return;
    m_tray->setIcon(makeTrayIcon(throttling));
    m_tray->setToolTip(throttling
                           ? QStringLiteral("Flowbit - limiting to %1 down / %2 up")
                                 .arg(formatRate(m_engine->downBps()),
                                      formatRate(m_engine->upBps()))
                           : QStringLiteral("Flowbit - uncapped"));
}

// ---------------------------------------------------------------------------
// Data flow
// ---------------------------------------------------------------------------

void MainWindow::loadFromStore()
{
    const Settings settings = m_store->load();

    m_loading = true;

    m_masterSwitch->setChecked(settings.enabled, false);

    const SpeedUnit dUnit = bestUnitFor(settings.defaultDown);
    const SpeedUnit uUnit = bestUnitFor(settings.defaultUp);
    m_downUnit->setCurrentIndex(m_downUnit->findData(int(dUnit)));
    m_upUnit->setCurrentIndex(m_upUnit->findData(int(uUnit)));

    m_downValue->setValue(fromBytesPerSec(settings.defaultDown, dUnit));
    m_upValue->setValue(fromBytesPerSec(settings.defaultUp, uUnit));

    // Seed the authoritative rates from settings before touching the sliders.
    m_downRate = settings.defaultDown;
    m_upRate = settings.defaultUp;
    if (settings.defaultDown > 0)
        ensureSliderCeiling(settings.defaultDown);
    if (settings.defaultUp > 0)
        ensureSliderCeiling(settings.defaultUp);

    m_downSlider->setValue(rateToSlider(settings.defaultDown));
    m_upSlider->setValue(rateToSlider(settings.defaultUp));
    m_downSlider->setUnlimited(settings.defaultDown <= 0);
    m_upSlider->setUnlimited(settings.defaultUp <= 0);

    m_quotaSpin->setValue(settings.usage.quotaBytes / (1024.0 * 1024 * 1024));

    // Reflect the saved appearance preference without re-triggering a save.
    // Both selectors are set, and the signal is blocked so loading a stored
    // preference is not mistaken for the user changing it.
    if (m_themeCombo) {
        const int idx = m_themeCombo->findData(int(settings.themeMode));
        if (idx >= 0) {
            QSignalBlocker b(m_themeCombo);
            m_themeCombo->setCurrentIndex(idx);
        }
    }
    if (m_pageThemeCombo) {
        const int idx = m_pageThemeCombo->findData(int(settings.themeMode));
        if (idx >= 0) {
            QSignalBlocker b(m_pageThemeCombo);
            m_pageThemeCombo->setCurrentIndex(idx);
        }
    }

    // Startup and closing. Blocked for the same reason as the theme selectors: these write
    // to the settings file and, in the startup case, to the registry, and loading a stored
    // preference must not look like the user asking for it again.
    if (m_startupCheck) {
        const QSignalBlocker b(m_startupCheck);
        m_startupCheck->setChecked(settings.startWithWindows);
    }
    if (m_closeCombo) {
        const QSignalBlocker b(m_closeCombo);
        const int idx = m_closeCombo->findData(settings.closeToTray ? 1 : 0);
        if (idx >= 0)
            m_closeCombo->setCurrentIndex(idx);
    }
    if (m_updateCheck) {
        const QSignalBlocker b(m_updateCheck);
        m_updateCheck->setChecked(settings.checkForUpdates);
    }
    refreshStartupNote();

    // Self-heal the startup registration, once per launch.
    //
    // Up to 1.0.0 the scheduled task could not be created at all: schtasks /TR re-split the
    // quoted executable path on the space in "Program Files", so every create failed and
    // only the Run key - which cannot elevate - was ever written. The setting looked "on"
    // while Windows started an unelevated copy or nothing at all.
    //
    // The definition is fixed in autostart.cpp; this repairs an installation that already
    // carries the broken state, rather than requiring the user to find the toggle and cycle
    // it. Flowbit is elevated at this point, which is exactly when the task can be created.
    // Deferred so the window paints before schtasks is spawned.
    if (settings.startWithWindows && autostart::activeMechanism() != QLatin1String("task")) {
        QTimer::singleShot(0, this, [this] {
            const Settings current = m_store->load();
            if (!current.startWithWindows)
                return;
            QString startupError;
            autostart::setEnabled(true, &startupError);
            refreshStartupNote();
        });
    }

    rebuildScheduleCards();
    setLimiterInputsEnabled(settings.enabled);

    m_loading = false;

    m_usage->load(settings.usage);

    // The automatic check, offered once at launch.
    //
    // Delayed deliberately: the first seconds after launch are spent making the driver ready
    // and applying the first limits, and a network request competing with that would be the
    // wrong priority. Nothing depends on the answer arriving quickly, and a failure stays
    // silent - the network being down is not something to interrupt anyone over.
    static bool updateCheckStarted = false;
    if (!updateCheckStarted && settings.checkForUpdates) {
        updateCheckStarted = true;
        QTimer::singleShot(5000, this, [this] {
            if (m_updater)
                checkForUpdatesNow(false);
        });
    }
}

void MainWindow::setLimiterInputsEnabled(bool enabled)
{
    // The spec asks for the limit fields to be visibly inert when the master
    // switch is off, so it is unambiguous that nothing is being enforced.
    m_downSlider->setEnabled(enabled);
    m_upSlider->setEnabled(enabled);
    m_downValue->setEnabled(enabled);
    m_upValue->setEnabled(enabled);
    m_downUnit->setEnabled(enabled);
    m_upUnit->setEnabled(enabled);
}

int MainWindow::minutesUntilStartFrom(const Rule &rule, const QDateTime &now)
{
    const QTime start = parseClock(rule.from);
    if (!start.isValid())
        return -1;

    // Asked first, and outside the day loop, because "am I inside it now" is not a question
    // about today's mask: the after-midnight tail of an overnight window belongs to the day
    // it started. Deriving it from today's mask made a Monday-only 22:00 -> 07:00 rule report
    // "starts in 6 days" at 02:00 on Tuesday while it was, in fact, active.
    if (ruleMatches(rule, now)) {
        const QTime end = parseClock(rule.to);
        QDateTime endsAt(now.date(), end);
        // Not yet reached today, so the window ends tomorrow.
        if (end <= now.time())
            endsAt = endsAt.addDays(1);
        return -int(now.secsTo(endsAt) / 60);
    }

    for (int dayOffset = 0; dayOffset <= 7; ++dayOffset) {
        const QDateTime day = now.addDays(dayOffset);
        if (!maskContainsDay(rule.days, day.date().dayOfWeek()))
            continue;

        const QDateTime candidate(day.date(), start);
        if (candidate <= now)
            continue;
        return int(now.secsTo(candidate) / 60);
    }
    return -1;
}

QString MainWindow::humaniseDuration(int minutes)
{
    if (minutes < 0)
        return QStringLiteral("unknown");
    if (minutes < 60)
        return QStringLiteral("%1m").arg(minutes);

    const int h = minutes / 60;
    const int m = minutes % 60;
    if (h >= 24) {
        const int d = h / 24;
        const int rh = h % 24;
        return rh ? QStringLiteral("%1d %2h").arg(d).arg(rh) : QStringLiteral("%1d").arg(d);
    }
    return m ? QStringLiteral("%1h %2m").arg(h).arg(m) : QStringLiteral("%1h").arg(h);
}

// Snapshot of the settings as the UI currently shows them.
//
// Rules are read from the STORE rather than from the on-screen cards: the cards
// are a view, and rebuildScheduleCards() derives them from the store, so there is
// exactly one source of truth for the rule list. Only the limiter controls are
// read back from widgets, because those are the fields the user edits in place.
Settings MainWindow::collect() const
{
    Settings settings = m_store->load();

    settings.enabled = m_masterSwitch && m_masterSwitch->isChecked();
    if (m_downValue)
        settings.defaultDown = toBytesPerSec(m_downValue->value(), downUnit());
    if (m_upValue)
        settings.defaultUp = toBytesPerSec(m_upValue->value(), upUnit());
    if (m_quotaSpin)
        settings.usage.quotaBytes = m_quotaSpin->value() * 1024.0 * 1024 * 1024;

    return settings;
}

// ---------------------------------------------------------------------------
// Schedule management
//
// Every operation addresses a rule by its stable id via ruleIndexById(). The
// previous implementation located rules by their position in the on-screen list
// and required the row itself to hold keyboard focus, which is why Remove
// silently did nothing: clicking the button moved focus to the button, so no row
// qualified as "selected" and the handler returned early.
// ---------------------------------------------------------------------------

// Builds one card for `rule` and wires it to the id-based handlers.
ScheduleCard *MainWindow::makeScheduleCard(const Rule &rule, QWidget *parent)
{
    auto *card = new ScheduleCard(rule, parent);
    connect(card, &ScheduleCard::editRequested, this, &MainWindow::onScheduleEditRequested);
    connect(card, &ScheduleCard::deleteRequested, this,
            &MainWindow::onScheduleDeleteRequested);
    connect(card, &ScheduleCard::enabledChanged, this,
            &MainWindow::onScheduleEnabledChanged);
    return card;
}

void MainWindow::rebuildScheduleCards()
{
    const Settings settings = m_store->load();
    const QDateTime now = QDateTime::currentDateTime();

    const auto fill = [&](QVector<ScheduleCard *> &cards, QVBoxLayout *layout) {
        // Destroy the old cards immediately rather than via deleteLater().
        // Deferred deletion leaves them parented until the event loop next spins,
        // so a rebuild would briefly show twice as many cards - and any code (or
        // test) counting them in the same pass would see the stale ones.
        for (ScheduleCard *card : std::as_const(cards)) {
            layout->removeWidget(card);
            card->setParent(nullptr);
            delete card;
        }
        cards.clear();

        for (const Rule &rule : settings.rules) {
            auto *card = makeScheduleCard(rule, layout->parentWidget());
            // "Active" means the rule is enabled AND its window currently covers
            // now - derived from the same ruleMatches() the engine uses, so the
            // card can never claim something the limiter is not doing.
            card->setActiveState(rule.enabled && ruleMatches(rule, now));
            layout->insertWidget(layout->count() - 1, card);
            cards.append(card);
        }
    };

    if (m_ruleLayout)
        fill(m_ruleRows, m_ruleLayout);
    if (m_schedPageLayout)
        fill(m_schedPageCards, m_schedPageLayout);

    // The dedicated page shows its empty state instead of an empty list.
    if (m_schedEmptyState)
        m_schedEmptyState->setVisible(settings.rules.isEmpty());
    if (m_schedPageScroll)
        m_schedPageScroll->setVisible(!settings.rules.isEmpty());

    refreshScheduleIndicators();
}

void MainWindow::refreshScheduleIndicators()
{
    const Settings settings = m_store->load();
    const QDateTime now = QDateTime::currentDateTime();
    const EffectiveLimits limits = evaluate(settings, now);

    // Which rule, if any, is driving the limiter right now.
    const int activeIndex = limits.ruleIndex;
    const bool hasActiveRule = settings.enabled && activeIndex >= 0
                               && activeIndex < settings.rules.size();

    QString text;
    if (!settings.enabled) {
        text = QStringLiteral("Limiting is switched off, so no schedule is being applied.");
    } else if (hasActiveRule) {
        const Rule &r = settings.rules.at(activeIndex);
        const QTime to = parseClock(r.to);
        text = QStringLiteral("Active: %1 \u2014 ends at %2")
                   .arg(r.name.isEmpty() ? QStringLiteral("Untitled") : r.name,
                        to.isValid() ? to.toString(QStringLiteral("HH:mm")) : r.to);
    } else {
        // Report the next start from the real rule set.
        QString soonest;
        int soonestMinutes = INT_MAX;
        for (const Rule &r : settings.rules) {
            if (!r.enabled)
                continue;
            const int mins = minutesUntilStartFrom(r, now);
            if (mins >= 0 && mins < soonestMinutes) {
                soonestMinutes = mins;
                soonest = r.name;
            }
        }
        if (soonest.isEmpty()) {
            text = QStringLiteral("Inactive: No scheduled limits currently applied");
        } else {
            text = QStringLiteral("Next: %1 \u2014 starts in %2")
                       .arg(soonest, humaniseDuration(soonestMinutes));
        }
    }

    if (m_schedPageStatus)
        m_schedPageStatus->setText(text);
    if (m_footerStatus)
        m_footerStatus->setText(text);

}

void MainWindow::onAddRule()
{
    // Suggested values: continue from the last rule's end, which is the usual
    // intent when adding a second window.
    Rule suggested;
    const Settings settings = m_store->load();
    suggested.name = QStringLiteral("Rule %1").arg(settings.rules.size() + 1);
    if (!settings.rules.isEmpty()) {
        const QTime end = parseClock(settings.rules.last().to);
        if (end.isValid()) {
            suggested.from = end.toString(QStringLiteral("HH:mm"));
            suggested.to = QTime((end.hour() + 2) % 24, 0).toString(QStringLiteral("HH:mm"));
        }
    }
    suggested.down = RateBps(2.0 * 1024 * 1024);
    suggested.up = RateBps(512 * 1024);

    ScheduleDialog dialog(suggested, /*isNew=*/true, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    addSchedule(dialog.result());
}

bool MainWindow::addSchedule(const Rule &rule)
{
    Settings settings = m_store->load();
    Rule stored = rule;
    if (stored.id == 0)
        stored.id = nextRuleId();
    settings.rules.append(stored);

    // Creating an enabled schedule also switches limiting on.
    //
    // This is a real usability defect that made the Scheduler look broken: the
    // master "Limiting" toggle on the Limiter page gates everything, and
    // evaluate() returns "no throttling" the moment it is off - so a schedule
    // could be created, shown as saved, and then never do anything. The only
    // hint was a line of status text that is easy to miss. Someone who has just
    // set up a time window plainly wants it to take effect.
    bool switchedOn = false;
    if (stored.enabled && !settings.enabled) {
        settings.enabled = true;
        switchedOn = true;
    }

    QString error;
    if (!m_store->save(settings, &error)) {
        statusBar()->showMessage(
            QStringLiteral("Could not save the schedule: %1").arg(error), 6000);
        return false;
    }

    if (switchedOn) {
        // Reflect it in the UI so the master switch is not silently out of step
        // with what was just saved.
        if (m_masterSwitch)
            m_masterSwitch->setChecked(true, false);
        setLimiterInputsEnabled(true);
        statusBar()->showMessage(
            QStringLiteral("Schedule saved, and bandwidth limiting switched on so it "
                           "can take effect."),
            6000);
    }

    rebuildScheduleCards();
    persistAndApply();
    statusBar()->showMessage(QStringLiteral("Schedule added"), 3000);
    return true;
}

bool MainWindow::updateSchedule(const Rule &rule)
{
    Settings settings = m_store->load();
    // Located by id, so an edit can never land on a different rule.
    const int index = ruleIndexById(settings, rule.id);
    if (index < 0) {
        statusBar()->showMessage(
            QStringLiteral("That schedule no longer exists"), 4000);
        return false;
    }

    settings.rules[index] = rule;

    QString error;
    if (!m_store->save(settings, &error)) {
        statusBar()->showMessage(
            QStringLiteral("Could not save the schedule: %1").arg(error), 6000);
        return false;
    }

    rebuildScheduleCards();
    persistAndApply();
    statusBar()->showMessage(QStringLiteral("Schedule updated"), 3000);
    return true;
}

bool MainWindow::removeSchedule(quint64 id)
{
    Settings settings = m_store->load();
    const int index = ruleIndexById(settings, id);
    if (index < 0) {
        statusBar()->showMessage(
            QStringLiteral("That schedule no longer exists"), 4000);
        return false;
    }

    settings.rules.removeAt(index);

    QString error;
    if (!m_store->save(settings, &error)) {
        statusBar()->showMessage(
            QStringLiteral("Could not delete the schedule: %1").arg(error), 6000);
        return false;
    }

    // Rebuild from the stored list rather than patching the widgets, so the UI
    // cannot drift from what was saved. An empty list is a legitimate state and
    // is shown as the empty state - nothing recreates a default rule.
    rebuildScheduleCards();

    // Recompute what should be applied now that this rule is gone.
    persistAndApply();

    statusBar()->showMessage(QStringLiteral("Schedule deleted"), 3000);
    return true;
}

void MainWindow::onScheduleEditRequested(quint64 id)
{
    const Settings settings = m_store->load();
    const int index = ruleIndexById(settings, id);
    if (index < 0) {
        statusBar()->showMessage(QStringLiteral("That schedule no longer exists"), 4000);
        return;
    }

    // The dialog is seeded with the rule's current values and keeps its id, so
    // saving updates in place instead of appending a duplicate.
    ScheduleDialog dialog(settings.rules.at(index), /*isNew=*/false, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    updateSchedule(dialog.result());
}

void MainWindow::onScheduleDeleteRequested(quint64 id)
{
    const Settings settings = m_store->load();
    const int index = ruleIndexById(settings, id);
    if (index < 0) {
        statusBar()->showMessage(QStringLiteral("That schedule no longer exists"), 4000);
        return;
    }

    const Rule &rule = settings.rules.at(index);
    const QString name = rule.name.isEmpty() ? QStringLiteral("this schedule") : rule.name;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Delete Schedule?"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("Are you sure you want to delete \"%1\"?").arg(name));
    box.setInformativeText(QStringLiteral("This action cannot be undone."));
    QPushButton *deleteButton = box.addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(qobject_cast<QPushButton *>(box.buttons().last()));
    box.exec();

    if (box.clickedButton() != deleteButton)
        return;

    removeSchedule(id);
}

void MainWindow::onScheduleEnabledChanged(quint64 id, bool enabled)
{
    Settings settings = m_store->load();
    const int index = ruleIndexById(settings, id);
    if (index < 0)
        return;

    if (settings.rules.at(index).enabled == enabled)
        return;

    settings.rules[index].enabled = enabled;

    QString error;
    if (!m_store->save(settings, &error)) {
        statusBar()->showMessage(
            QStringLiteral("Could not save the schedule: %1").arg(error), 6000);
        return;
    }

    // Only the indicators need refreshing: the cards already show the new state
    // because the toggle changed it. Rebuilding here would fight the user's click.
    refreshScheduleIndicators();
    persistAndApply();
}

void MainWindow::onCancelRule()
{
    // Retained for the dashboard's Cancel button; drafts no longer exist now that
    // adding happens in a modal dialog, so this simply hides the button.
    statusBar()->showMessage(QStringLiteral("Nothing to cancel"), 2000);
}

void MainWindow::onRemoveRule()
{
    // Retained as a no-op entry point: deletion is per-card now. The old
    // implementation required a focused row and silently refused otherwise.
    statusBar()->showMessage(
        QStringLiteral("Use the Delete button on the schedule you want to remove"), 3000);
}

void MainWindow::onRuleRowChanged()
{
    // A card reports changes through its own id-carrying signals, so this only
    // re-evaluates the effective limits.
    persistAndApply();
}

void MainWindow::persistAndApply()
{
    // Make the driver ready before anything tries to use it.
    //
    // Readiness checks actual driver access before considering service recovery:
    // WinDivert's temporary registration can read DISABLED while working normally.
    //
    // Once only: the condition changes when the files on disk change, not while the
    // application runs, and an SCM round trip on every settings change would be wasted work.
    static bool driverChecked = false;
    if (!driverChecked) {
        driverChecked = true;

        // A retry is armed by the Held case below, and disarmed as soon as the state clears or
        // the attempt budget runs out. Stopping it here means a manual apply - the user changing
        // a setting while a retry is pending - does not leave two paths racing to apply.
        if (m_driverRetry) {
            m_driverRetry->stop();
            m_driverAttempts = 0;
        }

        switch (m_engine->ensureDriverReady()) {
        case ThrottleEngine::DriverReadiness::Ready:
            break;   // nothing needed doing
        case ThrottleEngine::DriverReadiness::Repaired:
            // Worth recording, not worth interrupting anyone over: the user asked for a
            // limit and it will be applied. The diagnostics page reports the state.
            qInfo("Flowbit: the driver was ready to use after a repair at startup");
            break;
        case ThrottleEngine::DriverReadiness::Held:
            // Another instance still holds the driver. It becomes usable when that process
            // ends, which takes a moment - so this is a wait, not a fault.
            //
            // It goes to the status bar rather than the error strip. A red card is for something
            // the user has to read and act on; this resolves itself in a second or two, and
            // putting it in an alert made it look like a problem that needed solving. The
            // question "is that normal?" was asked about this message, which is the tell.
            //
            // The retry below is what makes the promise in the message true. It said "limits
            // will apply as soon as it exits" and previously nothing happened when it did.
            statusBar()->showMessage(
                QStringLiteral("Waiting for the previous copy of Flowbit to release the "
                               "network driver..."));
            armDriverRetry();
            break;
        case ThrottleEngine::DriverReadiness::NeedsRestart:
            m_errorLabel->setText(
                QStringLiteral("The WinDivert driver service is pending removal, which only "
                               "a restart of Windows clears. Limits will not apply until "
                               "then."));
            showErrorStrip();
            break;
        case ThrottleEngine::DriverReadiness::Unavailable:
            // No service, or one belonging to a different application's copy of WinDivert.
            // The engine's own open path explains the specific failure when it happens, so
            // nothing is said here: a second message about the same thing is noise.
            break;
        }
    }

    const Settings settings = collect();

    QString error;
    if (!m_store->save(settings, &error)) {
        m_errorLabel->setText(QStringLiteral("Could not save settings: %1").arg(error));
        showErrorStrip();
    }

    const EffectiveLimits limits = evaluate(settings, QDateTime::currentDateTime());

    // Everything speaks bytes/sec now, so this is a straight hand-off.
    m_engine->apply(limits.down, limits.up);
    m_graph->setCeiling(limits.down);
    refreshStatus();

    applyAppRules(settings);
}

// Hands the per-application rules to the limiter and refreshes the page.
//
// Deliberately separate from the system-wide hand-off above: this only ever feeds
// the app-limit engine, so nothing that happens here can disable, weaken or
// otherwise disturb the global limiter.
void MainWindow::applyAppRules(const Settings &settings)
{
    if (!m_appLimitEngine)
        return;

    m_appLimitEngine->setRules(settings.appRules);

    // The driver handles are only opened when there is something to enforce, so a
    // user who never adds an application pays nothing for the feature.
    const bool wantRunning = !settings.appRules.isEmpty();
    if (wantRunning && !m_appLimitEngine->isEnabled()) {
        m_appLimitEngine->setEnabled(true);
    } else if (!wantRunning && m_appLimitEngine->isEnabled()) {
        m_appLimitEngine->setEnabled(false);
    } else if (m_appLimitEngine->isEnabled()) {
        // Rules changed while running: recompute which are active right now.
        m_appLimitEngine->refresh();
    }

    if (m_appLimiterPageWidget) {
        m_appLimiterPageWidget->setRules(settings.appRules);
        m_appLimiterPageWidget->refresh();
    }

    // A failure to enforce is surfaced rather than swallowed: configured limits that
    // are not in force look identical to working ones, so silence would mislead.
    if (!settings.appRules.isEmpty() && !m_appLimitEngine->isRunning()
        && !m_appLimitEngine->errorString().isEmpty()) {
        m_errorLabel->setText(m_appLimitEngine->errorString());
        showErrorStrip();
    }
}

void MainWindow::refreshStatus()
{
    const Settings settings = collect();
    const EffectiveLimits limits = evaluate(settings, QDateTime::currentDateTime());

    if (!settings.enabled) {
        m_footerStatus->setText(QStringLiteral("Status: Off (traffic unrestricted)"));
        m_footerStatus->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextMuted().name()));
    } else if (limits.throttling) {
        m_footerStatus->setText(
            QStringLiteral("Status: Active (%1 applied)  \u2022  \u2193 %2  \u2191 %3")
                .arg(limits.reason, formatRate(limits.down), formatRate(limits.up)));
        // Green while a limit is genuinely in force, matching the toggle's cyan.
        m_footerStatus->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                          .arg(theme::kGreen().name()));
    } else {
        m_footerStatus->setText(QStringLiteral("Status: Enabled but uncapped (0 limits)"));
        m_footerStatus->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kAmber().name()));
    }

    if (m_trayEnabledAction && m_trayEnabledAction->isChecked() != settings.enabled) {
        QSignalBlocker blocker(m_trayEnabledAction);
        m_trayEnabledAction->setChecked(settings.enabled);
    }

    updateTrayIcon(settings.enabled && limits.throttling);

    m_footerAdapter->setText(
        QStringLiteral("Adapter: %1").arg(m_monitor->activeAdapterName()));
}

void MainWindow::refreshStatistics()
{
    const Settings settings = m_store->load();

    // A day's totals, summed from the per-day records the tracker keeps.
    const auto sumRange = [&](int days, quint64 *down, quint64 *up) {
        *down = 0;
        *up = 0;
        const QVector<UsageTracker::DayUsage> cycle = m_usage->cycleDays(settings.usage);
        const QDate today = QDate::currentDate();
        const QDate from = today.addDays(-(days - 1));
        for (const UsageTracker::DayUsage &d : cycle) {
            if (d.date < from || d.date > today)
                continue;
            *down += d.down;
            *up += d.up;
        }
    };

    const auto setCard = [&](QLabel *label, quint64 down, quint64 up) {
        if (!label)
            return;
        // Both figures in one card: the download is what people watch, and the
        // upload beside it costs nothing to show and is often the surprise.
        label->setText(QStringLiteral("%1\n\u2193 %2    \u2191 %3")
                           .arg(formatBytes(double(down + up)),
                                formatBytes(double(down)),
                                formatBytes(double(up))));
    };

    quint64 dToday = 0, uToday = 0;
    quint64 dWeek = 0, uWeek = 0;
    sumRange(1, &dToday, &uToday);
    sumRange(7, &dWeek, &uWeek);

    const quint64 dCycle = m_usage->cycleDownBytes();
    const quint64 uCycle = m_usage->cycleUpBytes();

    setCard(m_statToday, dToday, uToday);
    setCard(m_statWeek, dWeek, uWeek);
    setCard(m_statCycle, dCycle, uCycle);

    const quint64 quota = settings.usage.quotaBytes;
    const quint64 used = m_usage->cycleTotalBytes();
    if (m_statQuota)
        m_statQuota->setText(formatBytes(double(quota)));
    if (m_statRemaining) {
        m_statRemaining->setText(quota > used
                                     ? formatBytes(double(quota - used))
                                     : QStringLiteral("Quota reached"));
    }

    // The cycle's overall split, with each direction as a share of the total so the
    // proportions are readable without doing the arithmetic.
    if (m_statDownUp) {
        const quint64 total = dCycle + uCycle;
        if (total == 0) {
            m_statDownUp->setText(QStringLiteral("No traffic recorded this cycle yet."));
        } else {
            const double downPct = 100.0 * double(dCycle) / double(total);
            m_statDownUp->setText(
                QStringLiteral("\u2193 %1  (%2%)     \u2191 %3  (%4%)")
                    .arg(formatBytes(double(dCycle)),
                         QString::number(downPct, 'f', 0),
                         formatBytes(double(uCycle)),
                         QString::number(100.0 - downPct, 'f', 0)));
        }
    }

    // The per-application table, busiest first over the whole cycle.
    if (m_appUsageTable) {
        // The window is the CYCLE, not a fixed 30 days.
        //
        // The panel's own caption says "over the current cycle", and the remainder row below is
        // computed against the cycle totals - so asking for a 30-day window made the two disagree
        // whenever the cycle is not exactly 30 days: a shorter cycle included days from before it
        // (row totals too large, remainder clamped to zero, real gap hidden), a longer one silently
        // dropped days. Both windows are the cycle now.
        const int cycleDays = qMax(1, UsageTracker::cycleStartDate(settings.usage)
                                          .daysTo(QDate::currentDate())
                                      + 1);
        const QVector<UsageTracker::AppUsage> apps = m_usage->appUsageOverDays(cycleDays);
        // The toggle only makes sense when there is something hidden behind it.
        if (m_appUsageMore) {
            m_appUsageMore->setVisible(apps.size() > 10);
            m_appUsageMore->setText(m_appUsageShowAll
                                        ? QStringLiteral("Show Top 10")
                                        : QStringLiteral("View All (%1)").arg(apps.size()));
        }
        const int shown = qMin(apps.size(), m_appUsageShowAll ? apps.size() : 10);

        // What is left over, shown instead of silently missing.
        //
        // Measured on a real machine: the machine total for the day was 9,848 MB while the
        // attributed rows summed to 4,136 MB - 58% absent, and a user comparing against qBittorrent's
        // own "600 MB downloaded" read 182 MB and reasonably called the table wrong. The causes are
        // mundane (system processes carry no application, and for a while the monitor only ran while
        // the panel was open), but a table that simply omits them looks like a broken counter.
        //
        // The remainder is therefore its own row, labelled for what it is.
        quint64 attributedDown = 0;
        quint64 attributedUp = 0;
        for (const UsageTracker::AppUsage &a : apps) {
            attributedDown += a.down;
            attributedUp += a.up;
        }
        const quint64 cycleDown = m_usage->cycleDownBytes();
        const quint64 cycleUp = m_usage->cycleUpBytes();
        const quint64 restDown = cycleDown > attributedDown ? cycleDown - attributedDown : 0;
        const quint64 restUp = cycleUp > attributedUp ? cycleUp - attributedUp : 0;
        const quint64 rest = restDown + restUp;
        const bool hasRemainder = rest > (1u << 20);   // more than a megabyte is worth a row

        m_appUsageTable->setRowCount(shown + (hasRemainder ? 1 : 0));
        for (int row = 0; row < shown; ++row) {
            const UsageTracker::AppUsage &a = apps.at(row);
            const QString label = a.name.isEmpty() ? QFileInfo(a.path).fileName() : a.name;

            auto *nameItem = new QTableWidgetItem(label);
            nameItem->setIcon(appIcon(a.path, QFileInfo(a.path).fileName()));
            nameItem->setToolTip(QStringLiteral("%1\n%2").arg(label, a.path));
            m_appUsageTable->setItem(row, 0, nameItem);

            m_appUsageTable->setItem(row, 1, new QTableWidgetItem(
                                                formatBytes(double(a.total()))));
            m_appUsageTable->setItem(row, 2, new QTableWidgetItem(
                                                formatBytes(double(a.down))));
            m_appUsageTable->setItem(row, 3, new QTableWidgetItem(
                                                formatBytes(double(a.up))));
        }
        const int totalRows = shown + (hasRemainder ? 1 : 0);
        if (hasRemainder) {
            const int row = shown;
            auto *nameItem = new QTableWidgetItem(QStringLiteral("System and unattributed"));
            nameItem->setToolTip(
                QStringLiteral("Traffic the machine measured but could not put under an application: "
                               "system services (svchost, DNS, Windows Update), and any time when "
                               "monitoring was not running. It is not an error - it is the gap "
                               "between the machine total and the rows above."));
            nameItem->setForeground(QBrush(theme::kTextMuted()));
            m_appUsageTable->setItem(row, 0, nameItem);

            const quint64 values[3] = {rest, restDown, restUp};
            for (int c = 1; c < 4; ++c) {
                auto *item = new QTableWidgetItem(formatBytes(double(values[c - 1])));
                item->setForeground(QBrush(theme::kTextMuted()));
                m_appUsageTable->setItem(row, c, item);
            }
        }

        for (int c = 1; c < 4; ++c) {
            for (int r = 0; r < totalRows; ++r) {
                if (QTableWidgetItem *it = m_appUsageTable->item(r, c))
                    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            }
        }
        m_appUsageEmpty->setVisible(totalRows == 0);
        m_appUsageTable->setVisible(totalRows > 0);
        if (m_appUsageHint) {
            m_appUsageHint->setText(
                shown == 0
                    ? QStringLiteral(
                          "No per-application history yet. Applications appear here once "
                          "they transfer data while monitoring is running - open Show "
                          "More on the Dashboard to start it.")
                    : QStringLiteral("Recorded per application over the current cycle. "
                                     "Showing %1 of %2.")
                          .arg(shown).arg(apps.size()));
        }
    }
}

void MainWindow::refreshUsage()
{
    const Settings settings = m_store->load();

    const double total = m_usage->cycleTotalBytes();
    const double quota = std::max(1.0, settings.usage.quotaBytes);

    // Report both numbers in GB so the pair reads consistently ("142.5 GB /
    // 500 GB"). formatBytes() would scale a small total down to MB or B, which
    // makes the two sides of the slash look unrelated.
    constexpr double kGi = 1024.0 * 1024 * 1024;
    const double usedGb = total / kGi;
    const double quotaGb = quota / kGi;

    m_usageReading->setText(QStringLiteral("%1 GB / %2 GB")
                                .arg(usedGb, 0, 'f', usedGb < 10.0 ? 2 : 1)
                                .arg(quotaGb, 0, 'f', quotaGb < 10.0 ? 2 : 0));
    m_usageBar->setProgress(total / quota);

    m_usageSplitDown->setText(formatBytes(double(m_usage->cycleDownBytes())));
    m_usageSplitUp->setText(formatBytes(double(m_usage->cycleUpBytes())));
    const QDate start = settings.usage.cycleStart.isValid()
                            ? settings.usage.cycleStart : QDate::currentDate();
    const int elapsed = qMax(1, int(start.daysTo(QDate::currentDate())) + 1);
    const int length = qMax(elapsed, int(start.daysTo(m_usage->cycleEnd(settings.usage))) + 1);
    if (elapsed >= 2 && total > 0.0) {
        m_usageProjection->setText(
            QStringLiteral("At this rate: %1 by %2 \u00b7 %3 per day")
                .arg(formatBytes(total / elapsed * length),
                     m_usage->cycleEnd(settings.usage).toString(QStringLiteral("MMM d")),
                     formatBytes(total / elapsed)));
    } else {
        m_usageProjection->setText(
            QStringLiteral("At this rate: a projection appears after a full day of the cycle"));
    }

    const int days = m_usage->daysUntilReset(settings.usage);
    const QDate end = m_usage->cycleEnd(settings.usage);
    m_usageReset->setText(QStringLiteral("Resets in: %1 Day%2 (%3)")
                              .arg(days)
                              .arg(days == 1 ? QString() : QStringLiteral("s"))
                              .arg(end.toString(QStringLiteral("MMM d"))));

    // Keep the Statistics page in step. It reads the same tracker, so it is brought
    // up to date in the same call rather than on a timer of its own.
    refreshStatistics();
}

// ---------------------------------------------------------------------------
// Zone 3: slider <-> rate mapping
//
// The slider uses a piecewise scale: the first 60% covers 0..kLowBandBytes for
// fine control at the low end where it matters, the rest spans up to the current
// ceiling. The ceiling is dynamic (see ensureSliderCeiling) so a typed rate above
// the default 100 MB/s still maps to a real handle position instead of clamping.
// ---------------------------------------------------------------------------

RateBps MainWindow::sliderToRate(int sliderValue) const
{
    if (sliderValue <= 0)
        return kUnlimited;

    const double t = std::min(1.0, double(sliderValue) / kSliderSteps);
    if (t <= kLowBandFraction)
        return RateBps((t / kLowBandFraction) * kLowBandBytes);

    const double frac = (t - kLowBandFraction) / (1.0 - kLowBandFraction);
    return RateBps(kLowBandBytes + frac * (m_sliderCeilingBytes - kLowBandBytes));
}

int MainWindow::rateToSlider(RateBps rate) const
{
    if (rate <= 0)
        return 0;

    const double r = double(rate);
    int pos = 0;
    if (r <= kLowBandBytes) {
        pos = int((r / kLowBandBytes) * kLowBandFraction * kSliderSteps);
    } else {
        const double span = std::max(1.0, m_sliderCeilingBytes - kLowBandBytes);
        const double frac = std::min(1.0, (r - kLowBandBytes) / span);
        pos = int((kLowBandFraction + frac * (1.0 - kLowBandFraction)) * kSliderSteps);
    }

    // Any non-zero rate must land on at least the first step. Without this, a
    // small typed value (a few hundred bytes/sec, or a low KB/s figure) rounded
    // down to 0 and the slider stayed pinned at "uncapped" while the box showed
    // a real number - the two controls disagreeing.
    return std::max(1, std::min(kSliderSteps, pos));
}

void MainWindow::ensureSliderCeiling(RateBps rate)
{
    // Grow the ceiling until it can represent `rate`, doubling so resolution is
    // never finer than needed. Never shrink below the default, so the usual
    // 0..100 MB/s range keeps its resolution.
    if (double(rate) <= m_sliderCeilingBytes && m_sliderCeilingBytes >= kHighBandBytes)
        return;

    double ceiling = std::max(m_sliderCeilingBytes, kHighBandBytes);
    while (ceiling < double(rate) && ceiling < 1.0e15)
        ceiling *= 2.0;

    m_sliderCeilingBytes = ceiling;
}

SpeedUnit MainWindow::downUnit() const
{
    return SpeedUnit(m_downUnit->currentData().toInt());
}

SpeedUnit MainWindow::upUnit() const
{
    return SpeedUnit(m_upUnit->currentData().toInt());
}

void MainWindow::syncNumericalFromSlider(bool download)
{
    TrackSlider *slider = download ? m_downSlider : m_upSlider;
    QDoubleSpinBox *box = download ? m_downValue : m_upValue;
    QComboBox *unitBox = download ? m_downUnit : m_upUnit;

    const RateBps rate = sliderToRate(slider->value());
    slider->setUnlimited(rate <= 0);

    // Keep the physical-rate tracker in step, so a later unit switch re-renders
    // this value rather than a stale one.
    if (download)
        m_downRate = rate;
    else
        m_upRate = rate;

    if (rate <= 0) {
        QSignalBlocker b(box);
        box->setValue(0);
        return;
    }

    // Follow the magnitude being dragged so the number stays readable.
    const SpeedUnit unit = bestUnitFor(rate);
    const int idx = unitBox->findData(int(unit));
    if (idx >= 0 && unitBox->currentIndex() != idx) {
        QSignalBlocker b(unitBox);
        unitBox->setCurrentIndex(idx);
    }

    QSignalBlocker b(box);
    box->setValue(fromBytesPerSec(rate, unit));
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onMasterToggled(bool on)
{
    // Keep the limiter fields in step with the switch: disabled when off, so the
    // UI cannot imply that a limit is being enforced while the engine is idle.
    setLimiterInputsEnabled(on);

    if (m_loading)
        return;
    persistAndApply();
}

void MainWindow::onDownloadSlider(int value)
{
    if (m_loading)
        return;
    Q_UNUSED(value)
    syncNumericalFromSlider(true);
    m_applyDebounce->start();
}

void MainWindow::onUploadSlider(int value)
{
    if (m_loading)
        return;
    Q_UNUSED(value)
    syncNumericalFromSlider(false);
    m_applyDebounce->start();
}

void MainWindow::onNumericalEdited()
{
    if (m_loading)
        return;

    // Mirror only the direction the user actually edited. Re-syncing both would
    // drag the untouched slider around on every keystroke and fight the edit.
    QObject *from = sender();
    const bool onlyUp = (from == m_upValue);

    if (!onlyUp) {
        m_downRate = toBytesPerSec(m_downValue->value(), downUnit());
        syncSliderFromRate(true, m_downRate);
    }
    if (onlyUp || from == nullptr) {
        m_upRate = toBytesPerSec(m_upValue->value(), upUnit());
        syncSliderFromRate(false, m_upRate);
    }

    m_applyDebounce->start();
}

void MainWindow::syncSliderFromRate(bool download, RateBps rate)
{
    TrackSlider *slider = download ? m_downSlider : m_upSlider;

    // Make sure the scale can actually represent this rate before positioning
    // the handle, or the value would silently clamp.
    if (rate > 0)
        ensureSliderCeiling(rate);

    QSignalBlocker blocker(slider);

    if (rate <= 0) {
        slider->setUnlimited(true);
        slider->setValue(0);
        return;
    }

    slider->setUnlimited(false);
    slider->setValue(rateToSlider(rate));
}

void MainWindow::onUnitChanged()
{
    if (m_loading)
        return;

    // Re-render the *tracked physical rate* in the newly selected unit.
    //
    // Deliberately not derived from the slider: the slider is quantised to 1000
    // steps across its whole span, so round-tripping through it loses precision
    // (a typed 12.5 MB/s came back as 0.05 MB/s, a 99.9% error). The physical
    // rates below are the source of truth; the slider is only a view of them.
    const RateBps down = m_downRate;
    const RateBps up = m_upRate;

    m_loading = true;
    {
        QSignalBlocker bd(m_downValue);
        QSignalBlocker bu(m_upValue);
        m_downValue->setValue(down > 0 ? fromBytesPerSec(down, downUnit()) : 0.0);
        m_upValue->setValue(up > 0 ? fromBytesPerSec(up, upUnit()) : 0.0);
    }
    m_loading = false;

    m_applyDebounce->start();
}

void MainWindow::onPresetClicked()
{
    auto *button = qobject_cast<QPushButton *>(sender());
    if (!button)
        return;
    applyPreset(button->text());
}

void MainWindow::applyPreset(const QString &name)
{
    for (const Preset &preset : kPresets) {
        if (name != QString::fromLatin1(preset.label))
            continue;

        const RateBps down = RateBps(preset.downBytes);
        const RateBps up = RateBps(preset.upBytes);
        const SpeedUnit dUnit = bestUnitFor(down);
        const SpeedUnit uUnit = bestUnitFor(up);

        m_loading = true;
        m_downUnit->setCurrentIndex(m_downUnit->findData(int(dUnit)));
        m_upUnit->setCurrentIndex(m_upUnit->findData(int(uUnit)));
        m_downValue->setValue(fromBytesPerSec(down, dUnit));
        m_upValue->setValue(fromBytesPerSec(up, uUnit));
        m_downRate = down;
        m_upRate = up;
        ensureSliderCeiling(down);
        ensureSliderCeiling(up);
        m_downSlider->setUnlimited(false);
        m_upSlider->setUnlimited(false);
        m_downSlider->setValue(rateToSlider(down));
        m_upSlider->setValue(rateToSlider(up));

        // Applying a preset implies intent to limit.
        if (!m_masterSwitch->isChecked())
            m_masterSwitch->setChecked(true, true);
        m_loading = false;

        persistAndApply();
        statusBar()->showMessage(QStringLiteral("Applied preset \"%1\"").arg(name), 3000);
        return;
    }
}

void MainWindow::onEvaluate()
{
    if (m_loading)
        return;
    persistAndApply();
}

void MainWindow::onThroughput(double downBps, double upBps)
{
    m_downCounter->setText(theme::formatBytesPerSec(downBps));
    m_upCounter->setText(theme::formatBytesPerSec(upBps));

    m_graph->addSample(downBps, upBps);

    // The sampling interval can drift; the adapter's exact byte deltas remain
    // authoritative for quota totals regardless of how long the tick took.
    m_usage->addSample(m_monitor->lastDownBytes(), m_monitor->lastUpBytes());
}

void MainWindow::onUsageChanged()
{
    if (m_appPanel && m_appPanel->isAnimating())
        return; // counters stay live; revealFinished refreshes the labels later
    refreshUsage();
}

void MainWindow::onResetCycle()
{
    Settings settings = m_store->load();
    settings.usage.cycleStart = QDate::currentDate();

    // If this save fails the cycle start is not remembered and the next launch begins a cycle
    // again - the reset appears to have worked and then did not, so the failure is reported.
    if (!persistSettings(settings, QStringLiteral("the billing cycle")))
        return;

    m_usage->resetCurrentCycle(settings.usage);
    refreshUsage();

    statusBar()->showMessage(QStringLiteral("Quota cycle restarted from today"), 4000);
}

void MainWindow::onResetNow()
{
    const Settings settings = m_store->load();
    m_usage->resetCurrentCycle(settings.usage);
    refreshUsage();
    statusBar()->showMessage(QStringLiteral("Usage counters for this cycle cleared"), 4000);
}

void MainWindow::onEngineError(const QString &message)
{
    m_errorLabel->setText(message);
    showErrorStrip();
    if (m_tray)
        m_tray->showMessage(QStringLiteral("Flowbit"), message, QSystemTrayIcon::Warning);
}

void MainWindow::onEngineStateChanged(bool throttling)
{
    updateTrayIcon(throttling);
    refreshStatus();

    // A limit that is now being applied means whatever was reported has stopped being
    // true. The driver-service failure in particular is repaired automatically on the
    // next attempt, so its message must not outlive the fault - otherwise the window
    // keeps warning about a problem that no longer exists.
    if (throttling && m_engine && m_engine->isThrottling() && m_errorLabel
        && m_errorLabel->isVisible()) {
        m_errorLabel->clear();
        m_errorLabel->hide();
        if (m_errorStrip)
            m_errorStrip->hide();
        if (m_errorDismiss)
            m_errorDismiss->hide();
    }
}

void MainWindow::reapplyWidgetStyles()
{
    // Widgets given their OWN setStyleSheet() override the application sheet, and
    // the colour was baked into the string when they were built. Reinstalling the
    // global sheet does not touch them, so switching dark -> light used to leave
    // dark cards, dark rule rows and a dark scroll well on a white page.
    //
    // Every such widget is rebuilt here. Anything new that sets its own stylesheet
    // must be refreshed in this method too.
    for (QFrame *card : findChildren<QFrame *>(QStringLiteral("card")))
        card->setStyleSheet(theme::cardStyleSheet(QStringLiteral("card")));

    // The sidebar's brand mark bakes the plate colour into a pixmap, so it has to
    // be re-rendered rather than merely repainted.
    if (m_sidebar)
        m_sidebar->refreshTheme();

    const auto paintLabel = [](QLabel *l, const QColor &c, bool bold = false) {
        if (!l)
            return;
        l->setStyleSheet(bold ? QStringLiteral("color: %1; font-weight: 600;").arg(c.name())
                              : QStringLiteral("color: %1;").arg(c.name()));
    };

    // Live-speed counters: primary ink for download, secondary for upload.
    paintLabel(m_downCounter, theme::kTextPrimary());
    paintLabel(m_upCounter, theme::kTextSecondary());

    // Slider direction captions.
    paintLabel(m_downSliderCaption, theme::kTextPrimary(), true);
    paintLabel(m_upSliderCaption, theme::kTextSecondary(), true);

    // The limit sliders hold their accent as a member, so it has to be pushed in.
    if (m_downSlider)
        m_downSlider->setAccent(theme::kTextPrimary());
    if (m_upSlider)
        m_upSlider->setAccent(theme::kTextSecondary());

    // Scheduler well.
    if (m_ruleScroll) {
        m_ruleScroll->setStyleSheet(
            QStringLiteral("QScrollArea { background-color: %1; border: 1px solid %2; "
                           "border-radius: 8px; }")
                .arg(theme::kPanelSunken().name(), theme::kBorder().name()));
    }


    // Schedule cards bake the rule colour and the two direction glyphs into
    // per-widget stylesheets, so they need rebuilding on a theme change.
    for (ScheduleCard *card : std::as_const(m_ruleRows))
        card->refreshTheme();
    for (ScheduleCard *card : std::as_const(m_schedPageCards))
        card->refreshTheme();

    // Footer status colour depends on state, so re-derive it rather than
    // flattening it to one colour.
    refreshStatus();
}

void MainWindow::onThemeModeChanged()
{
    // The native title bar is painted by DWM, not by the palette, and does not follow a theme
    // change on its own. Without this, switching dark -> light left a dark bar over a light
    // window and vice versa.
    theme::applyTitleBar(this);
    if (m_loading)
        return;

    Settings settings = m_store->load();
    settings.themeMode = Settings::ThemeMode(m_themeCombo->currentData().toInt());

    persistSettings(settings, QStringLiteral("the theme"));

    theme::setMode(settings.themeMode == Settings::ThemeMode::Light
                       ? theme::Mode::Light
                       : (settings.themeMode == Settings::ThemeMode::Dark
                              ? theme::Mode::Dark
                              : theme::Mode::Auto));

    // The custom-painted widgets read colours at paint time, so they only need a
    // repaint. The application sheet is reinstalled by setMode(), but widgets
    // carrying their OWN stylesheet override are untouched by that, so they are
    // rebuilt explicitly - otherwise the previous theme's surfaces survive.
    reapplyWidgetStyles();

    update();
    if (centralWidget())
        centralWidget()->update();
    statusBar()->showMessage(
        QStringLiteral("Theme: %1").arg(m_themeCombo->currentText()), 3000);
}

void MainWindow::onAbout()
{
    // Branded about card: the Flowbit wordmark ("Flow" white + "bit" cyan), the
    // tagline, and the facts a user actually needs when something misbehaves -
    // elevation status and where their data lives.
    const Settings settings = m_store->load();

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("About Flowbit"));
    box.setIconPixmap(makeTrayIcon(false).pixmap(64, 64));

    QString body;
    // Two-tone wordmark using text values rather than an accent colour: the
    // interface is strictly monochrome, so the second half is rendered in the
    // muted grey instead of cyan.
    body += QStringLiteral(
        "<div style='font-size:20pt; font-weight:600;'>"
        "<span style='color:%1;'>Flow</span><span style='color:%2;'>bit</span></div>")
        .arg(theme::kTextPrimary().name(), theme::kTextMuted().name());
    body += QStringLiteral("<div style='color:%1; margin-bottom:10px;'>%2</div>")
                .arg(theme::kTextSecondary().name(),
                     QStringLiteral("Control Your Connection"));
    body += QStringLiteral("<div>Version %1</div>").arg(QApplication::applicationVersion());
    body += QStringLiteral(
                "<div style='color:%1; margin-top:10px;'>"
                "System-wide bandwidth control for Windows.<br>"
                "Shapes traffic with WinDivert and must run elevated.</div>")
                .arg(theme::kTextSecondary().name());
    body += QStringLiteral("<div style='color:%1; margin-top:10px;'>Settings &amp; usage data:<br>%2</div>")
                .arg(theme::kTextMuted().name(),
                     m_store->directory().toHtmlEscaped());

    if (m_store->isUsingLegacyDirectory()) {
        // Be explicit rather than silently reading a differently-named folder.
        body += QStringLiteral(
                    "<div style='color:%1; margin-top:8px;'>"
                    "Using data from your previous install, so existing settings and "
                    "usage history were preserved.</div>")
                    .arg(theme::kAmber().name());
    }

    box.setTextFormat(Qt::RichText);
    box.setText(body);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

void MainWindow::toggleFromTray(bool on)
{
    if (m_loading)
        return;
    m_masterSwitch->setChecked(on, true);
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
        showAndRaise();
}

void MainWindow::showAndRaise()
{
    show();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();
}

void MainWindow::showErrorStrip()
{
    // The one place the error strip is shown.
    //
    // Seven call sites set its text and made it visible, and they had drifted apart: some
    // started the auto-hide timer and some did not, so a message's lifetime depended on
    // which code path raised it. "Report copied to the clipboard" was one of the ones that
    // did not, which is why it stayed on screen indefinitely.
    //
    // Everything needed to show it correctly lives here, so a new caller cannot get it
    // partly right.
    if (!m_errorLabel || !m_errorStrip)
        return;

    // The container is shown, not only the label: it is the container that carries the card
    // background and the red accent, and it is hidden while there is nothing to say.
    m_errorStrip->show();
    m_errorStrip->setGraphicsEffect(nullptr);

    if (m_errorDismiss)
        m_errorDismiss->show();
    m_errorLabel->show();

    if (m_errorAutoHide) {
        // Any fade left over from a previous message must go, or the new text would appear
        // at whatever opacity the last animation finished on.
        m_errorLabel->setGraphicsEffect(nullptr);
        m_errorAutoHide->start();
    }
}

void MainWindow::armDriverRetry()
{
    if (!m_driverRetry)
        return;

    // Reset the budget whenever a fresh wait begins, so a second instance closing later still
    // gets the full window rather than inheriting a nearly-exhausted count.
    m_driverAttempts = 0;
    m_driverRetry->start(kDriverRetryMs);
}

void MainWindow::onDriverRetry()
{
    if (!m_engine)
        return;

    const ThrottleEngine::DriverReadiness state = m_engine->ensureDriverReady();

    switch (state) {
    case ThrottleEngine::DriverReadiness::Ready:
    case ThrottleEngine::DriverReadiness::Repaired:
        // Free. Apply what the user asked for, through the same path a settings change uses, so
        // the limits, the graph and the per-application rules all follow - not just the driver.
        m_driverRetry->stop();
        m_driverAttempts = 0;
        statusBar()->showMessage(
            QStringLiteral("The network driver is free again. Limits applied."), 4000);
        persistAndApply();
        return;

    case ThrottleEngine::DriverReadiness::NeedsRestart:
        // Waiting will not help: only a restart clears a service pending removal. Say so now
        // rather than polling out the full window first.
        m_driverRetry->stop();
        m_driverAttempts = 0;
        m_errorLabel->setText(
            QStringLiteral("The WinDivert driver service is pending removal, which only a "
                           "restart of Windows clears. Limits will not apply until then."));
        showErrorStrip();
        return;

    case ThrottleEngine::DriverReadiness::Held:
        break;   // still held; handled below

    case ThrottleEngine::DriverReadiness::Unavailable:
        // Nothing to wait for. The engine's own open path explains the specific failure when a
        // limit is applied, so saying more here would be a second message about the same thing.
        m_driverRetry->stop();
        m_driverAttempts = 0;
        return;
    }

    ++m_driverAttempts;
    if (m_driverAttempts >= kDriverRetryLimit) {
        // Out of patience. The state has not cleared and waiting longer will not clear it, so it
        // becomes a fault the user is told about rather than a wait that is quietly abandoned -
        // a limit that is not applying and no explanation is the worst of the outcomes.
        m_driverRetry->stop();
        m_driverAttempts = 0;
        m_errorLabel->setText(
            QStringLiteral("The WinDivert driver is still held by another copy of Flowbit after "
                           "%1 seconds, so limits are not being applied. Close any other Flowbit "
                           "windows, or restart Windows if that does not help.")
                .arg(kDriverRetryLimit * kDriverRetryMs / 1000));
        showErrorStrip();
        return;
    }

    m_driverRetry->start(kDriverRetryMs);
}

void MainWindow::uninstallFlowbit()
{
    // The uninstaller is written by the installer and lives beside the executable.
    const QString dir = QCoreApplication::applicationDirPath();
    const QString uninstaller = dir + QStringLiteral("/unins000.exe");

    if (!QFile::exists(uninstaller)) {
        // Running from a build folder, where there is no installer and so no uninstaller.
        // Saying so is better than a button that appears to do nothing.
        m_uninstallNote->setText(QStringLiteral(
            "No uninstaller was found beside Flowbit, so it was not installed by the setup "
            "program and cannot be removed from here. Delete the folder %1 to remove it.").arg(dir));
        m_uninstallNote->show();
        return;
    }

    const int answer = QMessageBox::warning(
        this, QStringLiteral("Uninstall Flowbit"),
        QStringLiteral("Remove Flowbit from this computer?\n\n"
                       "Flowbit will close and be removed. You will be asked whether to keep "
                       "your settings and usage history before anything is deleted."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;

    // Detached, because the uninstaller's first act is to ask this process to close - and it
    // cannot proceed until it has. Waiting for it here would deadlock: the child waits for the
    // parent to exit, and the parent waits for the child.
    //
    // The working directory is set to the installation folder: Inno's uninstaller reads its own
    // data relative to itself, and starting it from elsewhere can make it fail to find it.
    const bool started = QProcess::startDetached(uninstaller, QStringList(), dir);

    if (!started) {
        m_uninstallNote->setText(QStringLiteral(
            "The uninstaller could not be started. Run %1 by hand to remove Flowbit.")
                .arg(uninstaller));
        m_uninstallNote->show();
        return;
    }

    // Close without the tray confirmation, since the user has just confirmed the removal and
    // leaving Flowbit running would block the uninstaller from deleting its files.
    m_quittingForUninstall = true;
    close();
}

void MainWindow::resetEverything()
{
    if (!m_store)
        return;

    const Settings current = m_store->load();

    // The confirmation names the counts, taken from the settings rather than written into the
    // sentence. A warning that says what is actually there is far harder to click past than one
    // that says "this cannot be undone".
    const QString message =
        QStringLiteral("Reset Flowbit to a fresh install?\n\n"
                       "%1\n\n"
                       "Your 30-day usage figures and application history will also be deleted. "
                       "None of this can be undone.")
            .arg(SettingsStore::describeContents(current));

    const int answer = QMessageBox::warning(
        this, QStringLiteral("Reset everything"),
        message,
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!m_store->resetEverything(&error)) {
        m_errorLabel->setText(QStringLiteral("Could not reset: %1").arg(error));
        showErrorStrip();
        if (m_errorAutoHide)
            m_errorAutoHide->start();
        // Partial by definition - some files went. Saying so is better than reporting a
        // success that did not happen.
        return;
    }

    // The startup entry is in the registry rather than on disk, so resetEverything cannot reach
    // it. Removing both mechanisms here is what stops the settings page showing the box
    // unticked while Windows still starts Flowbit at every logon - the exact disagreement
    // between the file and the mechanism that the startup work went to some trouble to avoid.
    QString ignored;
    autostart::setEnabled(false, &ignored);

    // Write the defaults back, so the application is running what it will read next launch.
    // The file was removed above; this creates it fresh rather than leaving nothing behind.
    const Settings defaults;
    persistSettings(defaults, QStringLiteral("your settings"));

    // Apply the defaults to the running application rather than waiting for a restart.
    //
    // A reset that only took effect on the next launch would look broken: every page would
    // still show the old limits and schedules.
    loadFromStore();

    // The theme is a stored preference, so it has to be reapplied rather than only reloaded -
    // a user who had chosen dark and resets should see the default appearance immediately.
    theme::setMode(defaults.themeMode == Settings::ThemeMode::Light
                       ? theme::Mode::Light
                       : (defaults.themeMode == Settings::ThemeMode::Dark ? theme::Mode::Dark
                                                                         : theme::Mode::Auto));
    reapplyWidgetStyles();
    theme::applyApplicationTheme();

    m_engine->detach();
    m_graph->setCeiling(kUnlimited);
    refreshStatus();
    if (centralWidget())
        centralWidget()->update();

    statusBar()->showMessage(QStringLiteral("Flowbit has been reset to its default settings."),
                             5000);
}

bool MainWindow::persistSettings(const Settings &s, const QString &what)
{
    QString error;
    if (!m_store->save(s, &error)) {
        statusBar()->showMessage(
            QStringLiteral("Could not save %1: %2").arg(what, error), 8000);
        qWarning("Flowbit: could not save %s: %s", qPrintable(what), qPrintable(error));
        return false;
    }
    return true;
}

void MainWindow::applyStartupSetting()
{
    if (!m_startupCheck)
        return;

    const Settings s = m_store->load();

    // The registration itself runs on a worker thread.
    //
    // setEnabled() shells out to schtasks.exe and waits up to twenty seconds for it. On the GUI
    // thread that is a twenty-second freeze for a checkbox - and Qt's own QProcess documentation
    // is explicit that waitForFinished() blocks the calling thread. This mirrors what
    // AppMonitor::startAsync() already does for its own slow, external work.
    if (m_startupApplying) {
        m_startupPending = true;
        return;
    }
    m_startupApplying = true;
    m_startupPending = false;

    const bool want = s.startWithWindows;
    QThread *worker = QThread::create([this, want] {
        QString error;
        const bool ok = autostart::setEnabled(want, &error);
        QMetaObject::invokeMethod(
            this, [this, ok, error] { onStartupSettingApplied(ok, error); },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    worker->start();
}

void MainWindow::onStartupSettingApplied(bool ok, const QString &error)
{
    m_startupApplying = false;

    // A task usually creates fine; when policy or permissions forbid one, setEnabled falls back
    // to the Run key and reports that with this marker. The fallback still starts Flowbit, so
    // it is not a failure - but it does mean a consent prompt at logon instead of none, and
    // that difference is the whole reason for preferring a task. Saying so is more useful than
    // reporting success and leaving the user to wonder why logon still prompts.
    if (ok && error.startsWith(QLatin1String("__fallback__"))) {
        if (m_startupNote) {
            m_startupNote->setText(
                QStringLiteral("Flowbit will start at logon, but Windows would not accept a "
                               "scheduled task, so the administrator prompt will appear. "
                               "Reason: %1")
                    .arg(error.mid(12)));
            m_startupNote->show();
        }
        return;
    }

    if (!ok) {
        // Say what failed rather than leaving a ticked box that does nothing. The note is
        // used instead of a dialog because a startup entry that was refused is a preference
        // that did not take, not a fault the user must acknowledge.
        if (m_startupNote) {
            m_startupNote->setText(QStringLiteral("Could not change the Windows startup "
                                                  "setting: %1")
                                       .arg(error));
            m_startupNote->show();
        }
        return;
    }

    refreshStartupNote();

    // Replay a request that arrived while this one was running.
    if (m_startupPending) {
        m_startupPending = false;
        applyStartupSetting();
    }
}

void MainWindow::refreshStartupNote()
{
    if (!m_startupNote || !m_startupCheck)
        return;

    QStringList notes;

    if (m_startupCheck->isChecked()) {
        const QString mechanism = autostart::activeMechanism();

        if (mechanism.isEmpty()) {
            // The setting is on but nothing is registered. This happens when Flowbit has been
            // moved: the entry still points at the old location, so nothing will start.
            // Reporting it is more useful than a tick that quietly means nothing.
            notes << QStringLiteral("The startup entry points at a different copy of Flowbit, "
                                    "so it will not start. Switch this off and on again to "
                                    "update it.");
        } else if (mechanism == QLatin1String("task")) {
            notes << QStringLiteral("Flowbit starts at logon with no administrator prompt.");
        } else {
            notes << QStringLiteral("Flowbit starts at logon. Windows will ask for "
                                    "administrator permission, because no scheduled task "
                                    "could be created.");
        }
    }

    // The conflict guard.
    //
    // Hiding to the tray is only safe while there is a tray to be reached by. Without one,
    // choosing to hide leaves Flowbit running - holding the driver and applying limits -
    // with nothing to click and no way to quit it short of Task Manager.
    //
    // So the choice is disabled rather than merely described. A note saying "this will not
    // work as you expect" still lets the user pick it; an unavailable entry does not.
    //
    // Disabled rather than removed, so the list does not change shape under the user and the
    // tooltip can say why. The stored preference is left alone: if the tray is temporarily
    // missing - Explorer restarting, a locked-down session - the user's choice is still their
    // choice and applies again once a tray exists.
    const bool trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
    if (m_closeCombo) {
        // Index 0 is "Minimise to the notification area".
        const QStandardItemModel *model =
            qobject_cast<QStandardItemModel *>(m_closeCombo->model());
        if (model) {
            if (QStandardItem *item = model->item(0)) {
                item->setEnabled(trayAvailable);
                item->setToolTip(trayAvailable
                                     ? QStringLiteral("Flowbit keeps running after the "
                                                      "window closes, so schedules still apply.")
                                     : QStringLiteral("Unavailable: Windows reports no "
                                                      "notification area, so Flowbit could "
                                                      "not be reached after closing."));
            }
        }

        if (!trayAvailable && m_closeCombo->currentData().toInt() != 0) {
            // The option is currently chosen and cannot be honoured, so the visible selection
            // is moved to the one that works. The saved value is untouched, so the choice
            // returns when a tray does.
            const QSignalBlocker b(m_closeCombo);
            const int idx = m_closeCombo->findData(0);
            if (idx >= 0)
                m_closeCombo->setCurrentIndex(idx);
        }
    }

    if (!trayAvailable) {
        notes << QStringLiteral("Windows is not offering a notification area, so Flowbit "
                                "cannot be hidden there: closing the window will quit it.");
    }

    if (notes.isEmpty()) {
        m_startupNote->hide();
        return;
    }
    m_startupNote->setText(notes.join(QLatin1Char(' ')));
    m_startupNote->show();
}

void MainWindow::setUpdateStatus(const QString &text, bool isError)
{
    // Switches the status label between the hint and error styles.
    //
    // The label reports a state that is usually fine and occasionally a failure, so the colour
    // has to follow the message. Done here rather than with two labels, so there is one place
    // the text is set and no chance of the two disagreeing.
    //
    // A member rather than a local lambda because the update handlers are connected with a
    // context object and can fire after checkForUpdatesNow has returned - anything they use
    // has to outlive that call.
    if (!m_updateStatus)
        return;

    const QString want = isError ? QStringLiteral("error") : QStringLiteral("hint");
    if (m_updateStatus->objectName() != want) {
        m_updateStatus->setObjectName(want);
        // A style change only takes effect after a re-polish, because the selector matches on
        // the object name.
        m_updateStatus->style()->unpolish(m_updateStatus);
        m_updateStatus->style()->polish(m_updateStatus);
    }
    m_updateStatus->setText(text);
    // The label is hidden at construction; without this every result stayed
    // invisible, including the one the user asked for.
    m_updateStatus->setVisible(!text.isEmpty());
}

void MainWindow::checkForUpdatesNow(bool announce)
{
    if (!m_updater) {
        if (announce)
            QMessageBox::information(this, QStringLiteral("Updates"),
                                     QStringLiteral("Update checking is not available in "
                                                    "this build."));
        return;
    }

    if (announce)
        setUpdateStatus(QStringLiteral("Checking..."), false);
    if (announce && m_checkNowButton)
        m_checkNowButton->setEnabled(false);

    // One-shot connections: the updater is a single object for the window's lifetime and a
    // second check would otherwise stack another set of handlers onto it.
    const auto restore = [this] {
        if (m_checkNowButton)
            m_checkNowButton->setEnabled(true);
    };

    auto *once = new QObject(this);
    connect(m_updater, &Updater::updateAvailable, once,
            [this, once, restore](const QString &version, const QString &url,
                                  const QString &notes) {
                restore();

                // A version the user skipped is not offered again. The setting existed and was
                // saved, but nothing ever read it, so "skip" could not work - the same version
                // came back on every launch with no way to stop it.
                const Settings current = m_store->load();
                if (!current.skippedVersion.isEmpty() && current.skippedVersion == version) {
                    setUpdateStatus(
                        QStringLiteral("Version %1 skipped. A newer version will be offered.")
                            .arg(version),
                        false);
                    once->deleteLater();
                    return;
                }

                setUpdateStatus(QStringLiteral("Version %1 is available.").arg(version), false);
                m_updater->offerUpdate(version, url, notes);
                once->deleteLater();
            });
    connect(m_updater, &Updater::upToDate, once,
            [this, once, restore, announce](const QString &version) {
                restore();
                setUpdateStatus(QStringLiteral("Up to date (%1).").arg(version), false);
                // Only said out loud when the user asked. At startup, "nothing to do" is
                // not worth a window.
                if (announce) {
                    QMessageBox::information(
                        this, QStringLiteral("Updates"),
                        QStringLiteral("Flowbit %1 is the newest version.").arg(version));
                }
                once->deleteLater();
            });
    connect(m_updater, &Updater::failed, once,
            [this, once, restore, announce](const QString &reason) {
                restore();
                setUpdateStatus(QStringLiteral("Could not check: %1").arg(reason), true);
                if (announce) {
                    QMessageBox::warning(
                        this, QStringLiteral("Updates"),
                        QStringLiteral("Could not check for updates.\n\n%1").arg(reason));
                }
                once->deleteLater();
            });

    m_updater->checkForUpdate();
}

void MainWindow::startHidden()
{
    // No window, but the tray icon must exist or the application would be running with
    // nothing to click.
    //
    // show() then hide() rather than doing nothing: the tray icon, its menu and its
    // tooltip are created by the window, and Qt only places the icon once the window has
    // been realised. Hiding immediately afterwards leaves the user with the tray entry
    // and no window, which is the intent.
    show();
    hide();

    if (m_tray) {
        m_tray->show();
        m_tray->showMessage(
            QStringLiteral("Flowbit"),
            QStringLiteral("Flowbit started with Windows and is limiting in the "
                           "background."),
            QSystemTrayIcon::Information, 4000);
    }
}

void MainWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    // The native caption is DWM's to draw, and it never follows the palette. Applying here
    // covers launch, and every later show covers theme switches - the call is idempotent and
    // cheap, and on Windows builds without the colour attributes it degrades to the dark-mode
    // flag alone.
    theme::applyTitleBar(this);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // The window goes away FIRST, unconditionally, before anything that can block.
    //
    // Every path through this function ends with the window hidden - either it is quitting, or
    // it is heading for the tray - so hiding up front costs nothing and guarantees the user
    // never watches a frozen window. It used to run a settings read, a usage flush and, on
    // quit, a synchronous engine and ETW teardown with the window still on screen, which is
    // the "press X and it sits there" report.
    //
    // The phases below are timed so that a report of "it still freezes" can be answered with a
    // number instead of another guess. The lines go to OutputDebugString (see
    // tools/debug_output_capture.cpp), which is the only channel a WIN32 GUI target has.
    QElapsedTimer phase;
    phase.start();
    const auto mark = [&phase](const char *what) {
        qInfo("Flowbit: close phase '%s' at %lld ms", what,
              static_cast<long long>(phase.elapsed()));
    };

    hide();
    mark("window hidden");

    const Settings settings = m_store->load();
    mark("settings loaded");

    const bool quitting = !settings.closeToTray || m_quittingForUninstall;

    // Bank usage on the way out. Even when the app keeps running in the tray, the user may
    // well shut the machine down next, and the periodic checkpoint could be up to 30
    // seconds stale.
    m_usage->flush();
    mark("usage flushed");

    // An uninstall closes the window whatever the tray setting says.
    //
    // "Keep running in the background after closing the window" is a preference about the close
    // button. It is not an instruction to survive an uninstall: the uninstaller cannot delete
    // Flowbit while Flowbit is running, and hiding to the tray would leave it waiting until the
    // user found the tray icon and quit by hand - with nothing on screen to say why the
    // uninstall had stopped.
    if (quitting) {
        // Close means close.
        //
        // Releasing the engine first is the important part: it stops the worker threads and
        // closes the WinDivert handle, so traffic stops being shaped. Simply quitting would
        // leave the driver holding packets until the process teardown reached it.
        // ThrottleEngine::detach() is the call that lets go of the driver; there is no
        // stop(), and detach() is what its own declaration describes. The per-application
        // engine is switched off rather than detached, which is its equivalent.
        if (m_engine)
            m_engine->detach();
        mark("system-wide engine detached");
        if (m_appLimitEngine)
            m_appLimitEngine->setEnabled(false);
        mark("application limiter released");
        if (m_appMonitor)
            m_appMonitor->stop();
        mark("ETW monitor stopped");

        // A tray icon that outlives the window would be a way to reach an application that
        // is on its way out.
        if (m_tray)
            m_tray->hide();

        QMainWindow::closeEvent(event);
        QApplication::quit();
        return;
    }

    // Hide to the tray and keep running, which is what Flowbit has always done.
    QMainWindow::closeEvent(event);
    if (settings.closeToTray && m_tray && m_tray->isVisible()) {
        // Say where the window went, ONCE - and once means once, not once per launch.
        //
        // This was a function-local static, so it reset with the process: a user who closes the
        // window daily saw the same notification daily. The flag is stored now, so the hint
        // appears on the first close after installation and never again.
        Settings hintSettings = m_store->load();
        if (!hintSettings.trayHintShown) {
            hintSettings.trayHintShown = true;
            QString hintError;
            persistSettings(hintSettings, QStringLiteral("your hintSettings"));
            m_tray->showMessage(
                QStringLiteral("Flowbit"),
                QStringLiteral("Flowbit keeps running in the notification area so "
                               "schedules still apply. Right-click the icon to quit."),
                QSystemTrayIcon::Information, 5000);
        }
    }

    // The window hides rather than exits, and monitoring deliberately KEEPS RUNNING.
    //
    // It used to be stopped here and "restarted when the panel is reopened". That contradicted
    // the rest of the design twice over: per-application usage is recorded from the monitor's
    // snapshot, so stopping it on every close meant usage stopped accumulating for as long as
    // the window stayed in the tray - which is most of the time.
    //
    // It was also the lag on close. AppMonitor::stop() is synchronous and waits for the ETW
    // consumer to return, so pressing X blocked the UI thread for up to a second before the
    // window settled. Nothing needs tearing down here: the monitor is meant to live as long as
    // the application does.
}

} // namespace net
