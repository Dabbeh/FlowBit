#pragma once

#include <QWidget>

class QTimer;

namespace net {

// A small monochrome "working" indicator: a row of dots that brighten in
// sequence. Used wherever the app is waiting for something it cannot hurry (the
// monitoring session starting, a scan in progress).
//
// Design notes:
//
//   * Monochrome, like every other icon in the app. A coloured spinner would
//     break the rail the same way the emoji gear did.
//   * Painted, not an animated image, so it stays sharp at any DPI and needs no
//     resource file.
//   * Driven by one QTimer that stops when the widget is hidden, so a waiting
//     indicator costs nothing when it is not on screen - which matters because
//     this app is often left running in the background while gaming.
class BusyDots : public QWidget
{
    Q_OBJECT

public:
    explicit BusyDots(QWidget *parent = nullptr);

    QSize sizeHint() const override;

    // Extra context shown after the dots, e.g. "Starting monitoring".
    void setLabel(const QString &label);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void tick();

    QTimer *m_timer = nullptr;
    int m_phase = 0;
    QString m_label;
};

} // namespace net
