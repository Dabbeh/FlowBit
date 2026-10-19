#pragma once

#include "settings.h"
#include "units.h"

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;

namespace net {

// Compact "exact limit + unit" editor used inside rule rows, so a scheduled
// window can carry its own down/up caps without leaving the table.
//
// Emits bytes/second, with 0 meaning uncapped.
class RateEditor : public QWidget
{
    Q_OBJECT

public:
    explicit RateEditor(const QColor &accent, QWidget *parent = nullptr);

    RateBps rate() const;
    void setRate(RateBps rate);

signals:
    void rateChanged(RateBps rate);

private slots:
    void onValueEdited();
    void onUnitChanged();

private:
    SpeedUnit unit() const;

    QColor m_accent;
    QDoubleSpinBox *m_value = nullptr;
    QComboBox *m_unit = nullptr;
    // Last known rate, so a unit switch can re-render rather than reinterpret.
    RateBps m_lastRate = kUnlimited;
    bool m_updating = false;
};

} // namespace net
