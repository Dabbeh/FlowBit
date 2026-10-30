#include "applimiterpage.h"

#include "appnames.h"
#include "schedule.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

namespace net {

namespace {

// "22:00 -> 07:00, Weekdays" - short enough for a card, complete enough to be
// trustworthy. Reuses the scheduler's own description logic so the wording matches
// the Scheduler page exactly.
QString windowSummary(const QVector<Rule> &windows, int index)
{
    if (windows.isEmpty())
        return QStringLiteral("Always on");

    const Rule &w = windows.at(index);
    QString s = QStringLiteral("%1 \u2192 %2").arg(w.from, w.to);
    if (w.days == 0x7F)
        s += QStringLiteral(", every day");
    else if (w.days == 0x1F)
        s += QStringLiteral(", weekdays");
    else if (w.days == 0x60)
        s += QStringLiteral(", weekends");
    else
        s += QStringLiteral(", %1 day(s)").arg(int(w.days));

    if (windows.size() > 1)
        s += QStringLiteral(" +%1 more").arg(windows.size() - 1);
    return s;
}

QString limitText(RateBps rate)
{
    return rate <= 0 ? QStringLiteral("Unlimited") : theme::formatBytesPerSec(double(rate));
}

} // namespace

// ---------------------------------------------------------------------------
// Card
// ---------------------------------------------------------------------------

AppLimitCard::AppLimitCard(const AppRule &rule, const AppLimitState &state,
                           bool engineRunning, QWidget *parent)
    : QFrame(parent), m_rule(rule), m_state(state), m_engineRunning(engineRunning)
{
    setObjectName(QStringLiteral("scheduleCard"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(6);

    // ---- header: icon, name, enable toggle --------------------------------
    auto *header = new QHBoxLayout();
    header->setSpacing(8);

    m_icon = new QLabel(this);
    m_icon->setFixedSize(20, 20);
    header->addWidget(m_icon);

    m_name = new QLabel(this);
    m_name->setObjectName(QStringLiteral("scheduleName"));
    header->addWidget(m_name);
    header->addStretch(1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("sub"));
    header->addWidget(m_status);

    m_enabled = new QPushButton(this);
    m_enabled->setObjectName(QStringLiteral("scheduleToggle"));
    m_enabled->setCheckable(true);
    m_enabled->setCursor(Qt::PointingHandCursor);
    connect(m_enabled, &QPushButton::toggled, this, [this](bool on) {
        emit enabledChanged(m_rule.id, on);
    });
    header->addWidget(m_enabled);

    root->addLayout(header);

    // ---- limits ------------------------------------------------------------
    auto *limits = new QHBoxLayout();
    limits->setSpacing(6);

    const auto addLimit = [&](const QColor &accent, QLabel **arrowOut, QLabel **valueOut,
                              const QString &caption) {
        auto *arrow = new QLabel(caption, this);
        arrow->setStyleSheet(QStringLiteral("color: %1;").arg(accent.name()));
        limits->addWidget(arrow);

        auto *value = new QLabel(this);
        value->setObjectName(QStringLiteral("limitValue"));
        limits->addWidget(value);

        *arrowOut = arrow;
        *valueOut = value;
    };

    addLimit(theme::kGraphDownload(), &m_downArrow, &m_downValue, QStringLiteral("\u2193"));
    limits->addSpacing(14);
    addLimit(theme::kGraphUpload(), &m_upArrow, &m_upValue, QStringLiteral("\u2191"));
    limits->addStretch(1);

    m_scheduleSummary = new QLabel(this);
    m_scheduleSummary->setObjectName(QStringLiteral("sub"));
    limits->addWidget(m_scheduleSummary);

    root->addLayout(limits);

    // ---- actions -----------------------------------------------------------
    auto *actions = new QHBoxLayout();
    actions->setSpacing(6);

    auto *edit = new QPushButton(QStringLiteral("Edit"), this);
    edit->setObjectName(QStringLiteral("ghost"));
    edit->setCursor(Qt::PointingHandCursor);
    connect(edit, &QPushButton::clicked, this,
            [this] { emit editRequested(m_rule.id); });
    actions->addWidget(edit);

    auto *schedule = new QPushButton(QStringLiteral("Schedule"), this);
    schedule->setObjectName(QStringLiteral("ghost"));
    schedule->setCursor(Qt::PointingHandCursor);
    schedule->setToolTip(QStringLiteral("Limit this application only during set hours"));
    connect(schedule, &QPushButton::clicked, this,
            [this] { emit scheduleRequested(m_rule.id); });
    actions->addWidget(schedule);

    auto *del = new QPushButton(QStringLiteral("Delete"), this);
    del->setObjectName(QStringLiteral("ghost"));
    del->setCursor(Qt::PointingHandCursor);
    connect(del, &QPushButton::clicked, this,
            [this] { emit deleteRequested(m_rule.id); });
    actions->addWidget(del);

    actions->addStretch(1);
    root->addLayout(actions);

    rebuild();
}

void AppLimitCard::setRule(const AppRule &rule, const AppLimitState &state, bool engineRunning)
{
    m_rule = rule;
    m_state = state;
    m_engineRunning = engineRunning;
    rebuild();
}

void AppLimitCard::refreshTheme()
{
    if (m_downArrow)
        m_downArrow->setStyleSheet(
            QStringLiteral("color: %1;").arg(theme::kGraphDownload().name()));
    if (m_upArrow)
        m_upArrow->setStyleSheet(
            QStringLiteral("color: %1;").arg(theme::kGraphUpload().name()));
    rebuild();
}

void AppLimitCard::rebuild()
{
    const QString title = m_rule.name.isEmpty() ? m_rule.exe : m_rule.name;
    if (m_name)
        m_name->setText(title);
    if (m_icon)
        m_icon->setPixmap(appIcon(m_rule.path, m_rule.exe).pixmap(20, 20));
    if (m_icon)
        m_icon->setToolTip(m_rule.path.isEmpty() ? m_rule.exe : m_rule.path);

    // This card describes the configured limits, including outside scheduled
    // hours. Prefer the active window; otherwise preview the first enabled
    // window (or the first saved window if all are disabled). Keep its hours
    // and rates together so multiple windows cannot produce a mismatched card.
    int shownWindow = m_state.matchedWindow;
    if (shownWindow < 0 || shownWindow >= m_rule.windows.size()) {
        shownWindow = m_rule.windows.isEmpty() ? -1 : 0;
        for (int i = 0; i < m_rule.windows.size(); ++i) {
            if (m_rule.windows.at(i).enabled) {
                shownWindow = i;
                break;
            }
        }
    }
    RateBps shownDown = m_rule.down;
    RateBps shownUp = m_rule.up;
    if (shownWindow >= 0) {
        const Rule &window = m_rule.windows.at(shownWindow);
        if (window.down != kUnlimited) shownDown = window.down;
        if (window.up != kUnlimited) shownUp = window.up;
    }
    if (m_state.active) {
        shownDown = m_state.down;
        shownUp = m_state.up;
    }

    if (m_downValue)
        m_downValue->setText(limitText(shownDown));
    if (m_upValue)
        m_upValue->setText(limitText(shownUp));

    if (m_enabled) {
        QSignalBlocker block(m_enabled);
        m_enabled->setChecked(m_rule.enabled);
        m_enabled->setText(m_rule.enabled ? QStringLiteral("Enabled")
                                          : QStringLiteral("Disabled"));
    }

    if (m_scheduleSummary)
        m_scheduleSummary->setText(windowSummary(m_rule.windows, shownWindow));
    const QString source = shownWindow < 0
        ? QStringLiteral("Original application limits")
        : QStringLiteral("Configured schedule limits. The status above shows whether they are currently active.");
    m_downValue->setToolTip(source);
    m_upValue->setToolTip(source);

    if (m_status) {
        // The status distinguishes three genuinely different situations, so a user
        // can tell "configured but switched off" from "not enforcing" from "working".
        QString text;
        bool warn = false;

        if (!m_rule.enabled) {
            text = QStringLiteral("Off");
        } else if (!m_engineRunning) {
            text = QStringLiteral("Not enforcing");
            warn = true;
        } else if (m_state.active) {
            text = QStringLiteral("Active");
        } else {
            text = m_state.reason.isEmpty() ? QStringLiteral("Idle") : m_state.reason;
        }

        m_status->setText(text);
        m_status->setStyleSheet(warn ? QStringLiteral("color: %1;")
                                           .arg(theme::kAmber().name())
                                     : QString());
    }
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

AppLimiterPage::AppLimiterPage(AppLimitEngine *engine, QWidget *parent)
    : QWidget(parent), m_engine(engine)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    // ---- header row: search, add ------------------------------------------
    auto *header = new QHBoxLayout();
    header->setSpacing(8);

    m_banner = new QLabel(this);
    m_banner->setObjectName(QStringLiteral("sub"));
    m_banner->setWordWrap(true);
    header->addWidget(m_banner, 1);

    m_addButton = new QPushButton(QStringLiteral("+ Add Application"), this);
    m_addButton->setCursor(Qt::PointingHandCursor);
    connect(m_addButton, &QPushButton::clicked, this, &AppLimiterPage::addRequested);
    header->addWidget(m_addButton);

    root->addLayout(header);

    // ---- scrollable card list ---------------------------------------------
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("appLimitScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_listHost = new QWidget();
    m_listLayout = new QVBoxLayout(m_listHost);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(8);
    m_listLayout->addStretch(1);
    m_scroll->setWidget(m_listHost);

    root->addWidget(m_scroll, 1);

    // ---- empty state -------------------------------------------------------
    m_emptyState = new QLabel(
        QStringLiteral("No applications are limited.\n\n"
                       "Add an application to control its download and upload speed "
                       "individually, without affecting anything else."),
        this);
    m_emptyState->setObjectName(QStringLiteral("sub"));
    m_emptyState->setAlignment(Qt::AlignCenter);
    m_emptyState->setWordWrap(true);
    root->addWidget(m_emptyState, 1);

    if (m_engine)
        connect(m_engine, &AppLimitEngine::stateChanged, this, &AppLimiterPage::refresh);

    refresh();
}

void AppLimiterPage::setRules(const QVector<AppRule> &rules)
{
    m_rules = rules;
    rebuildCards();
    refresh();
}

void AppLimiterPage::rebuildCards()
{
    // Destroy immediately rather than via deleteLater(): a deferred delete leaves
    // the old cards parented and visible for the rest of the event-loop pass, which
    // shows up as duplicated rows.
    for (AppLimitCard *card : m_cards) {
        m_listLayout->removeWidget(card);
        card->setParent(nullptr);
        delete card;
    }
    m_cards.clear();

    const bool running = m_engine && m_engine->isRunning();
    const QVector<AppLimitState> states = m_engine ? m_engine->states() : QVector<AppLimitState>{};

    for (const AppRule &rule : m_rules) {
        AppLimitState state;
        for (const AppLimitState &s : states) {
            if (s.ruleId == rule.id) {
                state = s;
                break;
            }
        }

        auto *card = new AppLimitCard(rule, state, running, m_listHost);
        connect(card, &AppLimitCard::editRequested, this, &AppLimiterPage::editRequested);
        connect(card, &AppLimitCard::deleteRequested, this, &AppLimiterPage::deleteRequested);
        connect(card, &AppLimitCard::scheduleRequested, this,
                &AppLimiterPage::scheduleRequested);
        connect(card, &AppLimitCard::enabledChanged, this,
                &AppLimiterPage::enabledChanged);
        // Inserted before the trailing stretch so cards stay pinned to the top.
        m_listLayout->insertWidget(m_cards.size(), card);
        m_cards.append(card);
    }
}

void AppLimiterPage::refresh()
{
    const bool hasCards = !m_rules.isEmpty();
    m_scroll->setVisible(hasCards);
    m_emptyState->setVisible(!hasCards);

    // The banner reports the enforcement state, which is the one thing a user must
    // not be misled about: configured limits that are not in force look identical to
    // working ones unless this is said plainly.
    QString banner;
    bool warn = false;

    if (m_engine && m_engine->isRunning()) {
        banner = QStringLiteral(
            "Limits are enforced alongside the system-wide limiter. An application's "
            "limit applies in addition to it, so the tighter of the two wins.");
        // Attribution may run in a slower mode on builds that hide the driver's
        // flow layer. Say so here rather than leaving the user to wonder why a
        // brand-new connection takes a moment to be picked up.
        if (!m_engine->attributionNote().isEmpty())
            banner += QLatin1Char(' ') + m_engine->attributionNote();
    } else if (m_engine && !m_engine->isEnabled()
               && m_engine->errorString().isEmpty()) {
        // Configured but never switched on. Distinct from a failure.
        banner = QStringLiteral(
            "Application limits are configured but not being enforced. Turn them on "
            "to apply them.");
        warn = true;
    } else if (m_engine && !m_engine->errorString().isEmpty()) {
        banner = m_engine->errorString();
        warn = true;
    } else {
        banner = QStringLiteral("Application limits are not running.");
        warn = true;
    }

    m_banner->setText(banner);
    m_banner->setStyleSheet(warn
                                ? QStringLiteral("color: %1;").arg(theme::kAmber().name())
                                : QString());

    // Keep every card's status line truthful, which may have changed without the
    // rules themselves changing.
    const bool running = m_engine && m_engine->isRunning();
    const QVector<AppLimitState> states = m_engine ? m_engine->states() : QVector<AppLimitState>{};
    for (AppLimitCard *card : m_cards) {
        for (const AppRule &rule : m_rules) {
            if (rule.id != card->ruleId())
                continue;
            AppLimitState state;
            for (const AppLimitState &s : states) {
                if (s.ruleId == rule.id) {
                    state = s;
                    break;
                }
            }
            card->setRule(rule, state, running);
            break;
        }
    }
}

} // namespace net
