#include "rateeditor.h"

#include "theme.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QSignalBlocker>

#include <cmath>

namespace net {

RateEditor::RateEditor(const QColor &accent, QWidget *parent)
    : QWidget(parent), m_accent(accent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_value = new QDoubleSpinBox(this);
    m_value->setRange(0.0, 100000.0);
    m_value->setDecimals(2);
    m_value->setSingleStep(0.25);
    m_value->setToolTip(QStringLiteral("Limit for this rule. 0 = uncapped."));
    // Editable like the master boxes: typed input must work, focus must stick,
    // and the value should commit on Enter or focus-out rather than on every
    // keystroke.
    m_value->setReadOnly(false);
    m_value->setFocusPolicy(Qt::StrongFocus);
    m_value->setKeyboardTracking(false);
    m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Remembered so refreshTheme() can rebuild the border after a theme change:
    // a per-widget stylesheet survives the application sheet being reinstalled.
    m_accent = accent;
    m_value->setObjectName(QStringLiteral("accentedSpin"));
    m_value->setStyleSheet(
        QStringLiteral("QDoubleSpinBox { border-left: 2px solid %1; }").arg(accent.name()));

    m_unit = new QComboBox(this);
    for (SpeedUnit u : allSpeedUnits())
        m_unit->addItem(speedUnitLabel(u), int(u));
    m_unit->setToolTip(QStringLiteral("Unit for this limit"));

    layout->addWidget(m_value, 1);
    layout->addWidget(m_unit);

    connect(m_value, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &RateEditor::onValueEdited);
    connect(m_unit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &RateEditor::onUnitChanged);

    setRate(kUnlimited);
}

SpeedUnit RateEditor::unit() const
{
    return SpeedUnit(m_unit->currentData().toInt());
}

RateBps RateEditor::rate() const
{
    return toBytesPerSec(m_value->value(), unit());
}

void RateEditor::setRate(RateBps rate)
{
    // Pick a readable unit for the magnitude, then show the value in it.
    const SpeedUnit u = rate > 0 ? bestUnitFor(rate) : SpeedUnit::MibPerSec;
    const int idx = m_unit->findData(int(u));

    m_updating = true;
    if (idx >= 0)
        m_unit->setCurrentIndex(idx);
    m_value->setValue(fromBytesPerSec(rate, u));
    m_lastRate = rate;
    m_updating = false;
}

void RateEditor::onValueEdited()
{
    if (m_updating)
        return;
    m_lastRate = rate();
    emit rateChanged(m_lastRate);
}

void RateEditor::onUnitChanged()
{
    if (m_updating)
        return;

    // The number in the box is expressed in the *previous* unit, so re-render the
    // last known rate in the newly selected unit. Reading rate() directly here
    // would reinterpret the old number in the new unit - a 1024x error when
    // switching between MB/s and KB/s.
    const SpeedUnit u = unit();
    const double converted = fromBytesPerSec(m_lastRate, u);

    m_updating = true;
    m_value->setValue(converted);
    m_updating = false;

    emit rateChanged(m_lastRate);
}

} // namespace net
