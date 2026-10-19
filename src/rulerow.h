#pragma once

#include "schedule.h"
#include "units.h"

#include <QWidget>

#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace net {

class DayPicker;

// One editable scheduled-rule row.
//
// Laid out as a single flat horizontal line with every control vertically
// centred:  [on] [name] [from] to [to] [days] .... [d/lmt/unit] [u/lmt/unit]
//
// Per-field captions are deliberately absent - the fields are self-describing
// (time boxes show HH:mm, the day chips show Mo..Su), and dropping them is what
// keeps the row compact.
//
// The row also REFLOWS. At the window's 850px minimum the single-line form needs
// ~792px but only ~768px is available, which pushed the limit controls past the
// right edge. Below a threshold the day chips move to their own line:
//
//   wide   [x] [name] [from] to [to] [days]        [down] [up]
//   narrow [x] [name] [from] to [to] [down] [up]
//          [days .......................................]
//
// The switch is driven by the row's own width (resizeEvent) rather than the
// window's, so it adapts to a resized window or a larger system font too.
class RuleRow : public QWidget
{
    Q_OBJECT

public:
    explicit RuleRow(const Rule &rule, QWidget *parent = nullptr);

    Rule toRule() const;
    void applyRule(const Rule &rule);

    // Draft rows are new and unconfirmed: they get a highlight and the parent
    // offers Cancel for them.
    void setDraft(bool draft);
    bool isDraft() const { return m_draft; }

    void setIndex(int index) { m_index = index; }

    // True when the row is currently using the two-line form (exposed for tests).
    bool isCompact() const { return m_compact; }

    // Rebuilds the row's per-widget stylesheets after a theme change. The frame
    // background and the muted limit arrows are baked into stylesheet strings at
    // construction, so without this they keep the previous theme's colours.
    void refreshTheme();

signals:
    void changed();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    SpeedUnit unitOf(QComboBox *box) const;
    void onUnitChanged(bool download);
    void emitChanged();
    void updateResponsiveLayout();

    int m_index = 0;
    bool m_draft = false;
    bool m_loading = false;
    bool m_compact = false;
    bool m_arranged = false;

    QCheckBox *m_enabled = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_from = nullptr;
    QLineEdit *m_to = nullptr;
    QLabel *m_sep = nullptr;
    DayPicker *m_days = nullptr;
    QDoubleSpinBox *m_downValue = nullptr;
    QComboBox *m_downUnit = nullptr;
    QDoubleSpinBox *m_upValue = nullptr;
    QComboBox *m_upUnit = nullptr;

    // Two alternative bodies inside the frame; exactly one is visible. Movable
    // widgets are reparented between them on reflow.
    QWidget *m_frame = nullptr;
    QWidget *m_wideRow = nullptr;      // single-line form
    QWidget *m_compactBody = nullptr;  // two-line form
    QWidget *m_daysHost = nullptr;
    QWidget *m_limitsHost = nullptr;
    QHBoxLayout *m_wide = nullptr;
    QHBoxLayout *m_narrowTop = nullptr;
    QHBoxLayout *m_bottomLay = nullptr;
    QVBoxLayout *m_compactOuter = nullptr;

    std::array<RateBps, 2> m_lastRate{kUnlimited, kUnlimited};
};

} // namespace net
