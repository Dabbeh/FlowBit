#pragma once

#include "settings.h"

#include <QColor>
#include <QVector>
#include <QWidget>

namespace net {

// Scrolling 60-second throughput graph.
//
// Download is drawn as a filled translucent cyan "mountain"; upload is an orange
// line hugging the bottom of the plot. A dashed horizontal line marks the active
// download cap so the user can see the ceiling the engine is holding them under.
//
// Samples arrive once per second from SpeedMonitor; the widget keeps a fixed
// 60-entry ring buffer and scales the Y axis to the largest value in view (with
// the cap included in that maximum so the ceiling line stays on screen).
class SpeedGraph : public QWidget
{
    Q_OBJECT

public:
    explicit SpeedGraph(QWidget *parent = nullptr);

    void addSample(double downBytesPerSec, double upBytesPerSec);

    // The cap currently in force; 0 (or negative) hides the ceiling line.
    void setCeiling(RateBps downCap);

    void clearHistory();

    // Mean of the samples currently in view, per direction. Public because it is the value the
    // graph draws as the average line, and a number drawn on screen should be checkable.
    double averageDown() const;
    double averageUp() const;

    // Pushes two label centre points apart until they are at least `minGap` apart, keeping both
    // inside [top, bottom]. Public and static so the rule can be tested without painting.
    //
    // Needed because the two averages are frequently within a few pixels of each other - an idle
    // machine has both near zero - and the labels then overprinted into an unreadable smear.
    static void separateLabelCentres(qreal *first, qreal *second, qreal minGap, qreal top,
                                     qreal bottom);

    // Taller by default, and free to grow: the card gives it stretch, so a maximised window
    // spends the extra height on the graph rather than leaving it fixed while everything else
    // grows.
    QSize sizeHint() const override { return QSize(420, 180); }
    QSize minimumSizeHint() const override { return QSize(240, 110); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawGrid(QPainter &p, const QRectF &plot) const;
    void drawMountain(QPainter &p, const QRectF &plot, double maxValue) const;
    void drawCeiling(QPainter &p, const QRectF &plot, double maxValue) const;
    void drawAverages(QPainter &p, const QRectF &plot, double maxValue) const;

    void drawAxisLabels(QPainter &p, const QRectF &plot, double maxValue) const;

    static constexpr int kCapacity = 60;

    QVector<double> m_down;   // ring buffer, oldest first after normalisation
    QVector<double> m_up;
    RateBps m_ceiling = kUnlimited;
};

} // namespace net
