#pragma once

#include <QColor>
#include <QWidget>

namespace net {

// Thin multi-segment progress bar used by the 30-day usage panel.
//
// The quota is split into discrete segments so partial progress reads at a
// glance; the filled colour shifts to amber then red as the quota is consumed.
class SegmentedBar : public QWidget
{
    Q_OBJECT

public:
    explicit SegmentedBar(QWidget *parent = nullptr);

    void setProgress(double fraction);   // 0.0 .. 1.0 (values > 1 clamp)
    void setSegments(int segments);

    QSize sizeHint() const override { return QSize(220, 10); }
    QSize minimumSizeHint() const override { return QSize(80, 10); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double m_fraction = 0.0;
    int m_segments = 20;
};

} // namespace net
