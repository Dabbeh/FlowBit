#pragma once

#include "applimitengine.h"
#include "settings.h"

#include <QFrame>
#include <QWidget>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace net {

// One application's card on the App Limiter page.
//
// Read-only like the schedule cards: values are shown, and every change goes
// through an explicit Edit. That keeps a click from silently altering a limit, and
// means the card cannot drift out of step with the stored rule.
class AppLimitCard : public QFrame
{
    Q_OBJECT

public:
    AppLimitCard(const AppRule &rule, const AppLimitState &state, bool engineRunning,
                 QWidget *parent = nullptr);

    quint64 ruleId() const { return m_rule.id; }
    void setRule(const AppRule &rule, const AppLimitState &state, bool engineRunning);

    // Re-applies theme-dependent colours. Called on a live theme switch; the card
    // bakes the cyan/orange arrows into per-widget stylesheets, which would
    // otherwise keep the previous theme's ink.
    void refreshTheme();

signals:
    void editRequested(quint64 id);
    void deleteRequested(quint64 id);
    void enabledChanged(quint64 id, bool enabled);
    void scheduleRequested(quint64 id);

private:
    void rebuild();

    AppRule m_rule;
    AppLimitState m_state;
    bool m_engineRunning = false;

    QLabel *m_icon = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_downArrow = nullptr;
    QLabel *m_downValue = nullptr;
    QLabel *m_upArrow = nullptr;
    QLabel *m_upValue = nullptr;
    QLabel *m_scheduleSummary = nullptr;
    QPushButton *m_enabled = nullptr;
};

// The App Limiter page.
//
// Lists only the applications the user has added. Flowbit never enumerates
// installed software here: discovery happens inside the Add dialog, on demand.
//
// When the engine cannot enforce - normally because Flowbit is not elevated - the
// page says so at the top rather than presenting limits that are not in force.
class AppLimiterPage : public QWidget
{
    Q_OBJECT

public:
    AppLimiterPage(AppLimitEngine *engine, QWidget *parent = nullptr);

    // Rebuilds the cards from `rules`. Called whenever the stored rules change.
    void setRules(const QVector<AppRule> &rules);

    // Refreshes the state banners and per-card status. Cheap; safe to call often.
    void refresh();

    int cardCount() const { return m_cards.size(); }

signals:
    void addRequested();
    void editRequested(quint64 id);
    void deleteRequested(quint64 id);
    void enabledChanged(quint64 id, bool enabled);
    void scheduleRequested(quint64 id);

private:
    void rebuildCards();

    AppLimitEngine *m_engine = nullptr;
    QVector<AppRule> m_rules;

    QScrollArea *m_scroll = nullptr;
    QWidget *m_listHost = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QVector<AppLimitCard *> m_cards;

    QLabel *m_banner = nullptr;
    QLabel *m_emptyState = nullptr;
    QPushButton *m_addButton = nullptr;
};

} // namespace net
