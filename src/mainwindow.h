#pragma once

#include "settings.h"
#include "units.h"

#include <QMainWindow>
#include <QSystemTrayIcon>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QPushButton;
class QPropertyAnimation;
class QScrollArea;
class QShowEvent;
class QStackedWidget;
class QTableWidget;
class QTimer;
class QVBoxLayout;
class QAction;

namespace net {

class ThrottleEngine;
class SpeedMonitor;
class SettingsStore;
class UsageTracker;
class SpeedGraph;
class TrackSlider;
class SegmentedBar;
class ToggleSwitch;
class Sidebar;
class AppMonitor;
class AppActivityPanel;
class AppLimitEngine;
class AppLimiterPage;
class DiagnosticsPanel;
class ScheduleCard;

// The "Modern-Retro Obsidian" dashboard, laid out in four zones:
//
//   +-----------------------------+----------------------+
//   | Live Speed (60s graph)      | 30-Day Data Usage    |
//   +-----------------------------+----------------------+
//   | Active Speed Limiter (sliders, units, presets)     |
//   +----------------------------------------------------+
//   | Time Scheduler (rule rows) + status footer         |
//   +----------------------------------------------------+
//
// Settings are applied as soon as they are edited: every change is persisted
// and pushed straight into the engine, matching qBittorrent's feel.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(SettingsStore *store, ThrottleEngine *engine, SpeedMonitor *monitor,
                        UsageTracker *usage, QWidget *parent = nullptr);
    ~MainWindow() override;

    void showAndRaise();

    // Writes the Run entry to match the checkbox, and reports a refusal in the note beside
    // it rather than in a dialog: the setting is a preference, and a modal box for a
    // registry write that did not happen would be heavier than the failure deserves.
    void applyStartupSetting();

    // Completion handler for the off-thread startup-task change.
    void onStartupSettingApplied(bool ok, const QString &error);

    // Saves settings and reports a failure where the user can see it.
    //
    // Several call sites discarded the result, so a full disk or a permissions problem made a
    // setting look applied while it silently reverted at the next launch. `what` names the thing
    // that could not be saved ("the theme", "your schedule") so the message says something.
    bool persistSettings(const Settings &s, const QString &what);

    // True while a schtasks call is in flight. schtasks can take seconds; waiting for it on the
    // GUI thread froze the window for up to the 20s timeout, which is why it now runs elsewhere.
    bool m_startupApplying = false;

    // A request that arrived while one was running, replayed afterwards so a rapid toggle cannot
    // be silently dropped.
    bool m_startupPending = false;

    // Runs the update check and reports the outcome. `announce` is false for the silent
    // check at startup, where "up to date" is not worth a window, and true when the user
    // pressed the button and is owed an answer either way.

    // Re-states the note under the startup controls: where the Run entry points, and
    // whether hiding to the tray is actually possible right now.
    void refreshStartupNote();

    // Sets the update status line, choosing the hint or error style to match the message.
    void setUpdateStatus(const QString &text, bool isError);

    // Starts with no window shown, for a launch from the Run key. The tray icon is placed
    // instead, so the application is reachable without opening anything.
    void startHidden();

protected:
    // Closing the window keeps the app in the tray, but any buffered usage must
    // be banked before that - a shutdown is not a crash, and losing the last
    // checkpoint on a normal close would be inexcusable.
    void closeEvent(QCloseEvent *event) override;

    // Paints the native title bar in the theme's colours.
    //
    // The caption is drawn by DWM from its own state and never follows the Qt palette, so
    // without this the bar stays black over a charcoal window. Applied on every show: the
    // first call covers launch, later ones cover theme switches and monitor changes - the
    // call is cheap and setting the same colour twice costs nothing.
    void showEvent(QShowEvent *event) override;

private slots:
    void onMasterToggled(bool on);
    void onDownloadSlider(int value);
    void onUploadSlider(int value);
    void onNumericalEdited();
    void onUnitChanged();
    void onPresetClicked();
    void onAddRule();
    void onCancelRule();
    void onRemoveRule();
    void onRuleRowChanged();
    void onEvaluate();
    void onThroughput(double downBps, double upBps);
    void onUsageChanged();
    void onResetCycle();
    void onResetNow();
    void onEngineError(const QString &message);
    void onEngineStateChanged(bool throttling);
    void onAbout();
    void onThemeModeChanged();
    void toggleFromTray(bool on);
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    void buildUi();
    QFrame *makeCard(const QString &title, QWidget *parent);
    QWidget *buildLiveSpeedCard(QWidget *parent = nullptr);
    QWidget *buildUsageCard(QWidget *parent = nullptr);
    QWidget *buildLimiterCard(QWidget *parent = nullptr);
    QWidget *buildSchedulerCard(QWidget *parent = nullptr);
    void applyAppRules(const Settings &settings);
    void buildTray();
    void loadFromStore();

    // -- page construction --------------------------------------------------
    // Each page is built once and kept alive in the stack, so switching pages
    // never rebuilds widgets or resets live state (the graph keeps its samples,
    // the usage figures keep updating while another page is showing).
    QWidget *makePage(const QString &heading, const QString &subheading, QWidget **bodyOut);
    QWidget *buildDashboardPage();
    QWidget *buildSchedulerPage();
    QWidget *buildAppLimiterPage();
    QWidget *buildStatisticsPage();
    QWidget *buildSettingsPage();

    // Per-application limiting. Public slots for the same reason the schedule ones
    // are: they are the page's entry points and tests drive them by name.
public slots:
    // Switches to a page. Connected to the sidebar's selection signal, and public for the
    // same reason: anything that wants to show a page asks the window, rather than emitting
    // the sidebar's signal and hoping something is listening.
    void onPageSelected(int page);

    // Runs an update check. nnounce is false for the silent check at launch, where a
    // network failure must not interrupt anyone, and true when the user pressed the button
    // and is owed an answer either way.
    void checkForUpdatesNow(bool announce);

    // Returns Flowbit to a fresh install, after confirming.
    //
    // Destructive and irreversible, so it lives on the Settings page only and is guarded by a
    // dialog that names the counts it is about to remove.
    void resetEverything();

    // Runs the installer's uninstaller, detached, and closes Flowbit so it can proceed.
    void uninstallFlowbit();

    // Starts the driver readiness retry, and rechecks it on each tick.
    //
    // When the driver becomes free the limits are applied through persistAndApply(), so the whole
    // sequence runs rather than a driver check alone.
    void armDriverRetry();
    void onDriverRetry();

    // The one place the error strip is shown, so its styling and its auto-hide timer cannot
    // drift apart between callers.
    void showErrorStrip();

    void onAppRuleAdd();
    void onAppRuleEdit(quint64 id);
    void onAppRuleDelete(quint64 id);
    void onAppRuleEnabledChanged(quint64 id, bool enabled);
    void onAppRuleSchedule(quint64 id);

private:

    // -- schedule management ------------------------------------------------
    // Rebuilds the schedule cards from the stored rules. Delete and edit both go
    // through the rule's stable id, never a list index.
    //
    // These are the window's public schedule API: the cards, the dialog and the
    // tests all drive rule changes through them, so the paths cannot diverge.
    void rebuildScheduleCards();
    ScheduleCard *makeScheduleCard(const Rule &rule, QWidget *parent);

public slots:
    bool addSchedule(const Rule &rule);
    bool updateSchedule(const Rule &rule);
    bool removeSchedule(quint64 id);
    void onScheduleEnabledChanged(quint64 id, bool enabled);
    void onScheduleEditRequested(quint64 id);
    void onScheduleDeleteRequested(quint64 id);

private:
    // Minutes until the given rule next starts; negative when it is already
    // running, and -1 when it can never match. Used for the "Next:" line, so a
    // rule that never fires shows nothing rather than a fabricated countdown.
    static int minutesUntilStartFrom(const Rule &rule, const QDateTime &now);
    static QString humaniseDuration(int minutes);

    // Applies the enable/disable state to both the dashboard summary and the
    // dedicated page, from the same underlying settings.
    void refreshScheduleIndicators();

    // Rebuilds the scrollable rule list from settings.
    void rebuildRuleRows();

    // Rebuilds every PER-WIDGET stylesheet after a theme change.
    //
    // setMode() reinstalls the application-wide sheet, but a widget given its own
    // setStyleSheet() keeps that override - and because the colour was baked into
    // the string at construction, it survives a theme switch. That is what left
    // dark cards and dark scroll wells behind when switching dark -> light.
    void reapplyWidgetStyles();

    // Collects the rule rows currently on screen.
    QVector<Rule> collectRules() const;

    // Enables/disables the limiter inputs to reflect the master switch.
    void setLimiterInputsEnabled(bool enabled);

    Settings collect() const;
    // One eased viewport glide for the dashboard. Used by the Show More/Show Less
    // flow AFTER the panel tween has finished, so the target is computed against
    // the final scroll range. Replaces any glide still in flight.
    void glideDashboardScrollTo(int target, int milliseconds);

    void persistAndApply();
    void refreshStatus();
    // Fills the Statistics page from the usage tracker. The page's labels were
    // created with an em-dash and never assigned before this existed.
    void refreshStatistics();

    void refreshUsage();
    void applyPreset(const QString &name);
    void updateTrayIcon(bool throttling);
    QIcon makeTrayIcon(bool throttling) const;

    // Slider <-> rate mapping. Sliders use a piecewise scale so the low end
    // (KB/s) gets useful resolution while still reaching tens of MB/s.
    RateBps sliderToRate(int sliderValue) const;
    int rateToSlider(RateBps rate) const;

    // Grows the slider scale if a typed rate exceeds the current ceiling, so the
    // handle position stays truthful instead of clamping.
    void ensureSliderCeiling(RateBps rate);

    // Mirrors a rate onto one slider without re-entering its valueChanged.
    void syncSliderFromRate(bool download, RateBps rate);

    SpeedUnit downUnit() const;
    SpeedUnit upUnit() const;
    void syncNumericalFromSlider(bool download);

    SettingsStore *m_store = nullptr;
    ThrottleEngine *m_engine = nullptr;
    SpeedMonitor *m_monitor = nullptr;
    UsageTracker *m_usage = nullptr;

    // -- zone 1: live speed -------------------------------------------------
    QLabel *m_downCounter = nullptr;
    QLabel *m_upCounter = nullptr;
    SpeedGraph *m_graph = nullptr;
    // The caption labels above each counter and slider carry a per-widget
    // stylesheet, so they are tracked for re-styling on a theme change.
    QLabel *m_downCounterCaption = nullptr;
    QLabel *m_upCounterCaption = nullptr;
    QLabel *m_downSliderCaption = nullptr;
    QLabel *m_upSliderCaption = nullptr;

    // -- zone 2: usage ------------------------------------------------------
    QLabel *m_usageReading = nullptr;
    QLabel *m_usageReset = nullptr;
    SegmentedBar *m_usageBar = nullptr;
    QDoubleSpinBox *m_quotaSpin = nullptr;

    // -- zone 3: limiter ----------------------------------------------------
    ToggleSwitch *m_masterSwitch = nullptr;
    QComboBox *m_themeCombo = nullptr;
    TrackSlider *m_downSlider = nullptr;
    TrackSlider *m_upSlider = nullptr;
    QDoubleSpinBox *m_downValue = nullptr;
    QDoubleSpinBox *m_upValue = nullptr;
    QComboBox *m_downUnit = nullptr;
    QComboBox *m_upUnit = nullptr;

    // -- zone 4: scheduler --------------------------------------------------
    // The dashboard keeps a compact, read-only view of the schedules; the
    // dedicated Scheduler page owns full management. Both render from the same
    // stored rule list, so they cannot disagree.
    QScrollArea *m_ruleScroll = nullptr;
    QWidget *m_ruleList = nullptr;
    QVBoxLayout *m_ruleLayout = nullptr;
    QVector<ScheduleCard *> m_ruleRows;
    QLabel *m_footerStatus = nullptr;
    QLabel *m_footerAdapter = nullptr;

    // Dedicated Scheduler page.
    QScrollArea *m_schedPageScroll = nullptr;
    QWidget *m_schedPageList = nullptr;
    QVBoxLayout *m_schedPageLayout = nullptr;
    QVector<ScheduleCard *> m_schedPageCards;
    QLabel *m_schedPageStatus = nullptr;
    QWidget *m_schedEmptyState = nullptr;
    QPushButton *m_schedAddButton = nullptr;
    QPushButton *m_schedEmptyAddButton = nullptr;

    // -- navigation ---------------------------------------------------------
    Sidebar *m_sidebar = nullptr;
    QStackedWidget *m_pages = nullptr;
    QWidget *m_dashboardPage = nullptr;
    QWidget *m_schedulerPage = nullptr;
    QWidget *m_appLimiterPage = nullptr;
    QWidget *m_statisticsPage = nullptr;
    QWidget *m_settingsPage = nullptr;

    // Limiter page mirrors the dashboard limiter. Both edit the same settings, so
    // the page holds its own controls but never its own state.
    QComboBox *m_pageThemeCombo = nullptr;

    // Startup and closing. Created in buildSettingsPage(), refreshed by the same load path
    // as the rest of the page.
    QCheckBox *m_startupCheck = nullptr;
    QComboBox *m_closeCombo = nullptr;
    QCheckBox *m_updateCheck = nullptr;
    QPushButton *m_checkNowButton = nullptr;
    QLabel *m_updateStatus = nullptr;
    QLabel *m_startupNote = nullptr;

    // The reset control. Clears every setting, schedule, application rule and the usage
    // history, after a confirmation that names what will be lost.
    QPushButton *m_resetButton = nullptr;

    // The uninstall control, and the note it shows when there is no uninstaller to run.
    QPushButton *m_uninstallButton = nullptr;
    QLabel *m_uninstallNote = nullptr;

    // Set while the application is closing for an uninstall, so the tray confirmation is skipped.
    // Leaving Flowbit running would block the uninstaller from deleting its own files.
    bool m_quittingForUninstall = false;

    // The driver readiness retry.
    //
    // A previous instance holding the driver is a wait, not a fault - it clears in a second or
    // two as that process finishes unloading. A single check at startup cannot see it clear, so
    // the state is polled and the limits are applied when it does.
    //
    // Bounded. If the state does not clear it will not clear by waiting, and an unbounded poll
    // would be a background loop doing nothing for the life of the session.
    // Re-offers a postponed update later in the same session. One timer, restarted on each
    // deferral, so repeated "Later" clicks cannot queue several prompts.
    QTimer *m_updateSnooze = nullptr;

    // How long a postponed update waits before it is mentioned again.
    static constexpr int kUpdateSnoozeMs = 30 * 60 * 1000;

    QTimer *m_driverRetry = nullptr;
    int m_driverAttempts = 0;

    // How often the driver is rechecked, and how many times before the wait is abandoned.
    // Six seconds in total: far longer than a process teardown needs, and short enough that a
    // genuinely stuck state is reported rather than hidden behind a quiet wait.
    static constexpr int kDriverRetryMs = 750;
    static constexpr int kDriverRetryLimit = 8;

    // The update checker. Created with the window and kept, so a check can be started from
    // the settings page at any time without a new network manager each press.
    class Updater *m_updater = nullptr;   // net::Updater, declared in updater.h

    // Statistics page.
    QPushButton *m_appUsageMore = nullptr;
    QTableWidget *m_appUsageTable = nullptr;
    QLabel *m_appUsageEmpty = nullptr;
    QLabel *m_appUsageHint = nullptr;
    bool m_appUsageShowAll = false;

    QLabel *m_statToday = nullptr;
    QLabel *m_statWeek = nullptr;
    QLabel *m_statCycle = nullptr;
    QLabel *m_statQuota = nullptr;
    QLabel *m_statRemaining = nullptr;
    QLabel *m_statDownUp = nullptr;
    QLabel *m_settingsAdapter = nullptr;
    QLabel *m_settingsDataDir = nullptr;

    // -- application monitoring ---------------------------------------------
    // Owned by the window; the panel reads snapshots from it. The monitor runs
    // its trace session on its own thread and is started only while the panel is
    // expanded.
    // Per-application bandwidth limiting. Independent of the system-wide limiter:
    // the two compose, so an application's limit is the tighter of the two and the
    // global cap stays enforced for everything else.
    AppLimitEngine *m_appLimitEngine = nullptr;
    AppLimiterPage *m_appLimiterPageWidget = nullptr;
    // Recomputes which per-application rules are active. Schedule windows open and
    // close on wall-clock time, so this is driven by a timer as well as by edits.
    QTimer *m_appLimitTimer = nullptr;

    AppMonitor *m_appMonitor = nullptr;
    AppActivityPanel *m_appPanel = nullptr;
    QPushButton *m_showMoreButton = nullptr;
    // The dashboard's scroll container. Used to walk the viewport down to the
    // application activity panel when Show More expands it below the fold.
    QScrollArea *m_dashScroll = nullptr;
    // Smooth scroll to the revealed panel (an instant jump used to mask
    // the reveal animation entirely).
    QPropertyAnimation *m_dashScrollAnim = nullptr;

    // Drives the panel's value refresh while it is expanded.
    QTimer *m_appRefreshTimer = nullptr;
    // Syncs the Settings-page diagnostics with the monitor's live state.
    std::function<void()> m_syncDiagnostics;

    // Reads system state for the Settings page. Purely diagnostic: it never
    // modifies anything.
    DiagnosticsPanel *m_diagnosticsPanel = nullptr;

    QLabel *m_errorLabel = nullptr;

    // The card the message sits in. It carries the background, the border and the red accent, so
    // it is hidden while there is nothing to say - hiding only the label left an empty styled
    // card on screen permanently.
    QWidget *m_errorStrip = nullptr;
    // Dismisses the error banner; the banner had no way to be cleared before.
    QPushButton *m_errorDismiss = nullptr;

    // Clears the error strip after a delay. Restarted whenever the message changes, so the
    // delay is measured from the latest message rather than from when the strip appeared.
    QTimer *m_errorAutoHide = nullptr;

    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_trayEnabledAction = nullptr;

    QTimer *m_scheduleTimer = nullptr;
    QTimer *m_applyDebounce = nullptr;
    QTimer *m_usageFlushTimer = nullptr;
    bool m_loading = false;

    // Upper end of the slider scale. Starts at the default and grows if a typed
    // rate needs more room.
    double m_sliderCeilingBytes = 100.0 * 1024 * 1024;

    // The authoritative limits, in bytes/sec. The spin boxes and sliders are both
    // views of these: the slider is quantised, so round-tripping a precise typed
    // value through it would lose accuracy (and did, at up to 99.9% error).
    RateBps m_downRate = kUnlimited;
    RateBps m_upRate = kUnlimited;
};

} // namespace net
