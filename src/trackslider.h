#pragma once

#include <QColor>
#include <QWidget>

namespace net {

// Track-based slider with a coloured fill and an active knob.
//
// Qt's QSlider styling is inconsistent across platforms for this look, so the
// widget paints itself. The value is a plain integer range; callers map it to a
// speed using a non-linear scale so the whole 0 -> 100 MB/s span is reachable
// with useful resolution at the low end.
class TrackSlider : public QWidget
{
    Q_OBJECT

public:
    explicit TrackSlider(const QColor &accent, QWidget *parent = nullptr);

    // Replaces the accent colour. The accent is stored as a member, so a theme
    // change has to push the new value in - a repaint alone would keep painting
    // the previous theme's ink.
    void setAccent(const QColor &accent);

    int value() const { return m_value; }
    void setValue(int value);

    void setRange(int minimum, int maximum);

    // Renders an "Uncapped" state: track greyed, knob at the far left.
    void setUnlimited(bool unlimited);
    bool isUnlimited() const { return m_unlimited; }

    QSize sizeHint() const override { return QSize(240, 26); }
    QSize minimumSizeHint() const override { return QSize(120, 26); }

signals:
    void valueChanged(int value);
    void sliderReleased();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    int posToValue(int x) const;
    int valueToPos(int value) const;
    QRectF trackRect() const;

    QColor m_accent;
    int m_min = 0;
    int m_max = 1000;
    int m_value = 0;
    bool m_unlimited = true;
    bool m_dragging = false;
};

} // namespace net
