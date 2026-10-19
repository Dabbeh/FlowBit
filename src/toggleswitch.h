#pragma once

#include <QColor>
#include <QWidget>

namespace net {

// Pill-shaped ON/OFF switch: cyan backplate with a white knob on the right when
// on, grey backplate with the knob on the left when off.
//
// The knob position is a real Qt property so QPropertyAnimation can drive it.
// (An earlier version animated a plain member that was never declared as a
// property: the animation silently did nothing and the knob stayed wherever it
// started, which is why the switch appeared stuck on OFF.)
class ToggleSwitch : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal knobPos READ knobPos WRITE setKnobPos)

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    bool isChecked() const { return m_checked; }
    void setChecked(bool checked, bool animate = true);

    qreal knobPos() const { return m_knobPos; }
    void setKnobPos(qreal pos);

    QSize sizeHint() const override { return QSize(62, 30); }
    QSize minimumSizeHint() const override { return QSize(56, 28); }

signals:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    bool m_checked = false;
    qreal m_knobPos = 0.0;   // 0 = left/off, 1 = right/on
};

} // namespace net
