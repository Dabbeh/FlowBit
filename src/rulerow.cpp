#include "rulerow.h"

#include "daypicker.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace net {

namespace {

constexpr int kControlHeight = 26;
constexpr int kNameWidth = 104;
constexpr int kTimeWidth = 52;
constexpr int kLimitBoxWidth = 62;
constexpr int kLimitUnitWidth = 58;

// Below this the single-line form cannot fit, so the day chips drop to their own
// line.
//
// Measured, not guessed. Inside the scheduler card a row is given ~768px at the
// minimum window size, and the one-line form needs ~762px - it genuinely fits.
// The old threshold of 800 therefore reflowed rows that had room to spare, which
// doubled each row's height and is what made the strip feel cramped vertically
// while the limit boxes were pushed to the far right. The threshold now sits just
// below the real one-line requirement.
constexpr int kCompactThreshold = 750;

const SpeedUnit kRuleUnits[] = {SpeedUnit::KibPerSec, SpeedUnit::MibPerSec};

QLineEdit *makeTimeEdit(QWidget *parent)
{
    auto *edit = new QLineEdit(parent);
    edit->setAlignment(Qt::AlignCenter);
    edit->setMaxLength(5);
    edit->setFixedSize(kTimeWidth, kControlHeight);
    edit->setToolTip(QStringLiteral("HH:mm, e.g. 22:00 or 0700"));
    return edit;
}

// "arrow + spin box + unit". The arrows are muted, not accent-coloured: the
// schedule is a neutral surface, and colour is reserved for the places it
// carries meaning (the traffic graph, the master limits).
QWidget *makeLimitGroup(const QString &arrow, QWidget *parent, QDoubleSpinBox **valueOut,
                        QComboBox **unitOut)
{
    auto *group = new QWidget(parent);
    auto *h = new QHBoxLayout(group);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);

    auto *label = new QLabel(arrow, group);
    // Named so refreshTheme() can find it after a theme change. The colour still
    // has to be applied here because a per-widget stylesheet overrides the
    // application sheet, and these arrows are deliberately muted rather than
    // inheriting the row's default text colour.
    label->setObjectName(QStringLiteral("limitArrow"));
    label->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600;").arg(theme::kTextMuted().name()));
    label->setFixedWidth(11);
    label->setAlignment(Qt::AlignCenter);

    auto *value = new QDoubleSpinBox(group);
    value->setRange(0.0, 100000.0);
    value->setDecimals(0);
    value->setSingleStep(1.0);
    value->setFixedSize(kLimitBoxWidth, kControlHeight);
    value->setButtonSymbols(QAbstractSpinBox::NoButtons);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    value->setToolTip(QStringLiteral("Limit for this window. 0 = uncapped."));

    auto *unit = new QComboBox(group);
    for (SpeedUnit u : kRuleUnits)
        unit->addItem(speedUnitLabel(u), int(u));
    unit->setFixedSize(kLimitUnitWidth, kControlHeight);
    unit->setToolTip(QStringLiteral("Unit for the limit to the left"));

    h->addWidget(label);
    h->addWidget(value);
    h->addWidget(unit);

    *valueOut = value;
    *unitOut = unit;
    return group;
}

} // namespace

RuleRow::RuleRow(const Rule &rule, QWidget *parent) : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_frame = new QFrame(this);
    m_frame->setObjectName(QStringLiteral("ruleRow"));
    outer->addWidget(m_frame);

    // Two alternative bodies live inside the frame; exactly one is visible. Both
    // are built up front and the movable widgets are reparented between them on
    // reflow, which is simpler and more predictable than tearing down layouts.

    auto *stack = new QVBoxLayout(m_frame);
    stack->setContentsMargins(8, 6, 8, 6);
    stack->setSpacing(0);

    // ---- wide body --------------------------------------------------------
    m_wideRow = new QWidget(m_frame);
    m_wide = new QHBoxLayout(m_wideRow);
    m_wide->setContentsMargins(0, 0, 0, 0);
    m_wide->setSpacing(7);

    // ---- narrow body ------------------------------------------------------
    m_compactBody = new QWidget(m_frame);
    auto *compactOuter = new QVBoxLayout(m_compactBody);
    compactOuter->setContentsMargins(0, 0, 0, 0);
    compactOuter->setSpacing(5);

    auto *compactTop = new QWidget(m_compactBody);
    m_narrowTop = new QHBoxLayout(compactTop);
    m_narrowTop->setContentsMargins(0, 0, 0, 0);
    m_narrowTop->setSpacing(7);

    auto *compactBottom = new QWidget(m_compactBody);
    auto *bottomLay = new QHBoxLayout(compactBottom);
    bottomLay->setContentsMargins(0, 0, 0, 0);
    bottomLay->setSpacing(7);

    compactOuter->addWidget(compactTop);
    compactOuter->addWidget(compactBottom);

    m_daysHost = new QWidget(m_compactBody);
    auto *daysLay = new QHBoxLayout(m_daysHost);
    daysLay->setContentsMargins(0, 0, 0, 0);
    daysLay->setSpacing(0);
    m_days = new DayPicker(m_daysHost);
    daysLay->addWidget(m_days);

    m_limitsHost = new QWidget(m_compactBody);
    auto *limLay = new QHBoxLayout(m_limitsHost);
    limLay->setContentsMargins(0, 0, 0, 0);
    limLay->setSpacing(12);

    QDoubleSpinBox *downValue = nullptr;
    QComboBox *downUnit = nullptr;
    limLay->addWidget(makeLimitGroup(QStringLiteral("\u2193"), m_limitsHost, &downValue, &downUnit));

    QDoubleSpinBox *upValue = nullptr;
    QComboBox *upUnit = nullptr;
    limLay->addWidget(makeLimitGroup(QStringLiteral("\u2191"), m_limitsHost, &upValue, &upUnit));

    m_downValue = downValue;
    m_downUnit = downUnit;
    m_upValue = upValue;
    m_upUnit = upUnit;

    // The fields that move between the two bodies.
    m_enabled = new QCheckBox(m_wideRow);
    m_enabled->setToolTip(QStringLiteral("Include this rule when evaluating the schedule"));

    m_name = new QLineEdit(m_wideRow);
    m_name->setPlaceholderText(QStringLiteral("Name"));
    m_name->setFixedSize(kNameWidth, kControlHeight);
    m_name->setToolTip(QStringLiteral("A label for this window, e.g. \"Work Hours\""));

    m_from = makeTimeEdit(m_wideRow);
    m_to = makeTimeEdit(m_wideRow);

    // A literal "to", not a dash: it is unambiguous, and it matches the wording
    // the helper text uses.
    m_sep = new QLabel(QStringLiteral("to"), m_wideRow);
    m_sep->setObjectName(QStringLiteral("sub"));

    stack->addWidget(m_wideRow);
    stack->addWidget(m_compactBody);

    m_bottomLay = bottomLay;
    m_compactOuter = compactOuter;

    // -- wiring ------------------------------------------------------------
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool) { emitChanged(); });
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &) { emitChanged(); });
    connect(m_from, &QLineEdit::textEdited, this, [this](const QString &) { emitChanged(); });
    connect(m_to, &QLineEdit::textEdited, this, [this](const QString &) { emitChanged(); });
    connect(m_days, &DayPicker::maskChanged, this, [this](quint8) { emitChanged(); });
    connect(m_downValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { emitChanged(); });
    connect(m_upValue, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { emitChanged(); });
    connect(m_downUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { onUnitChanged(true); });
    connect(m_upUnit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { onUnitChanged(false); });

    applyRule(rule);
    updateResponsiveLayout();
}

void RuleRow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

void RuleRow::updateResponsiveLayout()
{
    const bool wantCompact = width() > 0 && width() < kCompactThreshold;
    if (wantCompact == m_compact && m_arranged)
        return;
    m_compact = wantCompact;
    m_arranged = true;

    // Strip every movable widget out of both bodies first, then re-add them into
    // the one that is about to be shown. Without this the widgets keep their old
    // parent and simply vanish from the new arrangement.
    const auto detach = [](QLayout *lay, QWidget *w) {
        if (lay && w)
            lay->removeWidget(w);
    };
    for (QWidget *w : {static_cast<QWidget *>(m_enabled), static_cast<QWidget *>(m_name),
                       static_cast<QWidget *>(m_from), static_cast<QWidget *>(m_sep),
                       static_cast<QWidget *>(m_to), m_daysHost, m_limitsHost}) {
        detach(m_wide, w);
        detach(m_narrowTop, w);
        detach(m_bottomLay, w);
    }

    if (wantCompact) {
        m_enabled->setParent(m_compactBody);
        m_name->setParent(m_compactBody);
        m_from->setParent(m_compactBody);
        m_sep->setParent(m_compactBody);
        m_to->setParent(m_compactBody);
        m_daysHost->setParent(m_compactBody);
        m_limitsHost->setParent(m_compactBody);

        m_narrowTop->addWidget(m_enabled, 0, Qt::AlignVCenter);
        m_narrowTop->addWidget(m_name, 0, Qt::AlignVCenter);
        m_narrowTop->addWidget(m_from, 0, Qt::AlignVCenter);
        m_narrowTop->addWidget(m_sep, 0, Qt::AlignVCenter);
        m_narrowTop->addWidget(m_to, 0, Qt::AlignVCenter);
        m_narrowTop->addStretch(1);
        m_narrowTop->addWidget(m_limitsHost, 0, Qt::AlignVCenter);

        m_bottomLay->addWidget(m_daysHost, 0, Qt::AlignVCenter);
        m_bottomLay->addStretch(1);

        m_wideRow->hide();
        m_compactBody->show();
    } else {
        m_enabled->setParent(m_wideRow);
        m_name->setParent(m_wideRow);
        m_from->setParent(m_wideRow);
        m_sep->setParent(m_wideRow);
        m_to->setParent(m_wideRow);
        m_daysHost->setParent(m_wideRow);
        m_limitsHost->setParent(m_wideRow);

        m_wide->addWidget(m_enabled, 0, Qt::AlignVCenter);
        m_wide->addWidget(m_name, 0, Qt::AlignVCenter);
        m_wide->addWidget(m_from, 0, Qt::AlignVCenter);
        m_wide->addWidget(m_sep, 0, Qt::AlignVCenter);
        m_wide->addWidget(m_to, 0, Qt::AlignVCenter);
        m_wide->addWidget(m_daysHost, 0, Qt::AlignVCenter);
        m_wide->addStretch(1);
        m_wide->addWidget(m_limitsHost, 0, Qt::AlignVCenter);

        m_compactBody->hide();
        m_wideRow->show();
    }

    setMinimumHeight(wantCompact ? 2 * kControlHeight + 22 : kControlHeight + 12);
    setMaximumHeight(wantCompact ? 2 * kControlHeight + 22 : kControlHeight + 12);

    if (m_frame->layout())
        m_frame->layout()->activate();
    updateGeometry();
}

SpeedUnit RuleRow::unitOf(QComboBox *box) const
{
    return SpeedUnit(box->currentData().toInt());
}

void RuleRow::onUnitChanged(bool download)
{
    if (m_loading)
        return;

    QDoubleSpinBox *box = download ? m_downValue : m_upValue;
    QComboBox *unitBox = download ? m_downUnit : m_upUnit;
    const RateBps rate = m_lastRate[download ? 0 : 1];

    const SpeedUnit u = unitOf(unitBox);
    const double converted = rate > 0 ? fromBytesPerSec(rate, u) : 0.0;

    m_loading = true;
    box->setValue(converted);
    m_loading = false;

    m_lastRate[download ? 0 : 1] = rate;
    emitChanged();
}

void RuleRow::applyRule(const Rule &rule)
{
    m_loading = true;
    m_enabled->setChecked(rule.enabled);
    m_name->setText(rule.name);
    m_from->setText(rule.from);
    m_to->setText(rule.to);
    m_days->setMask(rule.days);

    const SpeedUnit du = rule.down > 0 ? bestUnitFor(rule.down) : SpeedUnit::MibPerSec;
    const SpeedUnit uu = rule.up > 0 ? bestUnitFor(rule.up) : SpeedUnit::KibPerSec;
    m_downUnit->setCurrentIndex(m_downUnit->findData(int(du)));
    m_upUnit->setCurrentIndex(m_upUnit->findData(int(uu)));
    m_downValue->setValue(fromBytesPerSec(rule.down, du));
    m_upValue->setValue(fromBytesPerSec(rule.up, uu));

    m_lastRate[0] = rule.down;
    m_lastRate[1] = rule.up;
    m_loading = false;

    setDraft(m_draft);
}

Rule RuleRow::toRule() const
{
    Rule rule;
    rule.enabled = m_enabled->isChecked();
    rule.name = m_name->text().trimmed();
    rule.from = m_from->text().trimmed();
    rule.to = m_to->text().trimmed();
    rule.days = m_days->mask();
    rule.down = toBytesPerSec(m_downValue->value(), unitOf(m_downUnit));
    rule.up = toBytesPerSec(m_upValue->value(), unitOf(m_upUnit));
    return rule;
}

void RuleRow::setDraft(bool draft)
{
    m_draft = draft;

    // Draft state is shown with a lighter border only. No accent colour: the
    // schedule stays neutral, and a brighter outline is enough to read as
    // "not saved yet".
    const QString border = draft ? theme::kTextMuted().name() : theme::kBorder().name();
    if (QFrame *frame = qobject_cast<QFrame *>(m_frame)) {
        frame->setStyleSheet(QStringLiteral("QFrame#ruleRow { background-color: %1; "
                                            "border: 1px solid %2; border-radius: 7px; }")
                                 .arg(theme::kPanelRaised().name(), border));
    }
}

void RuleRow::refreshTheme()
{
    // The frame background and the muted limit arrows are baked into stylesheet
    // strings at construction. Reinstalling the application sheet does not touch a
    // widget that carries its own override, so both are rebuilt here - which is
    // what stops a rule row staying dark after switching to the light theme.
    for (QLabel *arrow : findChildren<QLabel *>(QStringLiteral("limitArrow"))) {
        arrow->setStyleSheet(
            QStringLiteral("color: %1; font-weight: 600;").arg(theme::kTextMuted().name()));
    }
    setDraft(m_draft);
}

void RuleRow::emitChanged()
{
    if (m_loading)
        return;
    emit changed();
}

} // namespace net
