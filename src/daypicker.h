#pragma once

#include <QWidget>

namespace net {

// Compact Monday-first weekday selector: seven small toggle chips.
//
// Emits a 7-bit mask with Monday = bit 0 ... Sunday = bit 6, matching Rule.days.
// A couple of one-click presets (Mon-Fri / Daily) sit alongside the chips, since
// "weekdays" and "every day" are by far the most common choices.
class DayPicker : public QWidget
{
    Q_OBJECT

public:
    explicit DayPicker(QWidget *parent = nullptr);

    quint8 mask() const { return m_mask; }
    void setMask(quint8 mask);

    // Display-only mode for schedule cards. Clicks are ignored so a saved rule
    // cannot be altered by an accidental click on its day chips; editing happens
    // through the rule dialog.
    void setReadOnly(bool readOnly);
    bool isReadOnly() const { return m_readOnly; }

    QSize sizeHint() const override;

signals:
    void maskChanged(quint8 mask);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    // Width of one chip, derived from the UI font so labels cannot clip.
    int chipWidth() const;

    QRectF chipRect(int index) const;
    int chipAt(const QPoint &pos) const;
    void toggle(int index);

    quint8 m_mask = 0x7F;
    bool m_readOnly = false;
    int m_hover = -1;
};

} // namespace net
