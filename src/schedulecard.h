#pragma once

#include "schedule.h"

#include <QFrame>

class QLabel;
class QPushButton;
class QHBoxLayout;

namespace net {

class DayPicker;

// One schedule, shown as a read-only card.
//
// This replaces the old editable RuleRow. The row tried to be a form and a list
// item at once: every field was permanently editable, controls were crammed onto
// one line until the unit dropdowns clipped, and deletion depended on which widget
// happened to hold focus. A card shows the configured values and acts through
// explicit Edit / Delete / Enable controls, each tied to the rule's stable id.
//
// The card reflows: at wide widths the details sit on one line with the actions
// on the right; when narrow, the actions drop to their own row so nothing is
// squeezed or truncated.
class ScheduleCard : public QFrame
{
    Q_OBJECT

public:
    explicit ScheduleCard(const Rule &rule, QWidget *parent = nullptr);

    quint64 ruleId() const { return m_rule.id; }
    const Rule &rule() const { return m_rule; }
    void setRule(const Rule &rule);

    // Human-readable summary of whether this rule is applying limits right now.
    void setActiveState(bool active);

    // Re-applies the colours that live in per-widget stylesheets. The rule name
    // and the two direction glyphs bake a colour at construction, so a live theme
    // switch would otherwise leave them at the previous theme's value - which
    // showed up as near-white text on the white page.
    void refreshTheme() { refreshTexts(); }

    bool isEnabledState() const { return m_rule.enabled; }
    bool isCompact() const { return m_compact; }

signals:
    void editRequested(quint64 id);
    void deleteRequested(quint64 id);
    void enabledChanged(quint64 id, bool enabled);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuildLayout();
    void refreshTexts();
    void applyEnabledLook();

    Rule m_rule;
    bool m_compact = false;
    bool m_arranged = false;
    bool m_active = false;
    // Guards the toggle's toggled() signal while it is being set
    // programmatically, so loading a rule does not look like a user edit.
    bool m_updating = false;

    QLabel *m_clock = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_scheduleLine = nullptr;
    QLabel *m_nextLine = nullptr;
    QLabel *m_downGlyph = nullptr;
    QLabel *m_downValue = nullptr;
    QLabel *m_upGlyph = nullptr;
    QLabel *m_upValue = nullptr;
    DayPicker *m_days = nullptr;
    QPushButton *m_toggle = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_delete = nullptr;

    QWidget *m_topRow = nullptr;
    QWidget *m_actions = nullptr;
    QHBoxLayout *m_root = nullptr;
    QHBoxLayout *m_top = nullptr;
    QHBoxLayout *m_actionRow = nullptr;

    static constexpr int kCompactThreshold = 720;
};

} // namespace net
