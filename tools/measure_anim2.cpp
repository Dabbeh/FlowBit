// Finds what makes the reveal animation drop a frame.
//
// Measurement of the real panel showed a 33px jump out of 203px of travel - about
// 16% of the distance in a single frame, where a smooth tween should manage a few
// percent. Everything else was correct (the panel's top edge did not move, and 15
// distinct heights were observed).
//
// Two candidates are tested here:
//
//   A. The rank timer (3s) and the value refresh (1s) run during the tween and do
//      real work - rebuilding a table - which delays an animation tick.
//   B. The animation's default update interval (16ms) beats against the sample
//      rate, so one frame is skipped and the next covers double the distance.

#include <QApplication>
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QTableWidgetItem>
#include <QElapsedTimer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <vector>

namespace {

// A widget whose height is animated exactly like the panel's, with an optional
// "expensive" task running on its own timer (standing in for the table rebuild).
class Tweened : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(int animHeight READ animHeight WRITE setAnimHeight)

public:
    explicit Tweened(bool withHeavyTimer, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(0);

        m_anim = new QPropertyAnimation(this, "animHeight", this);
        m_anim->setDuration(260);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);

        if (withHeavyTimer) {
            // Stand-in for AppActivityPanel::refresh(): a table rebuild.
            m_heavy = new QTimer(this);
            m_heavy->setInterval(1000);
            connect(m_heavy, &QTimer::timeout, this, &Tweened::doHeavyWork);
            m_heavy->start();
        }
    }

    int animHeight() const { return m_h; }

    void setAnimHeight(int h)
    {
        m_h = h;
        setMinimumHeight(qMax(1, h));
        setMaximumHeight(qMax(1, h));
    }

    void reveal()
    {
        m_anim->stop();
        m_anim->setStartValue(1);
        m_anim->setEndValue(203);
        setAnimHeight(1);
        m_anim->start();
    }

    void doHeavyWork()
    {
        // Roughly what rebuildTable() does each second: rebuild every row.
        if (!m_probe) {
            m_probe = new QTableWidget(12, 4, this);
            m_probe->setVisible(false);
        }
        m_probe->setRowCount(12);
        for (int r = 0; r < 12; ++r) {
            for (int c = 0; c < 4; ++c) {
                m_probe->setItem(r, c, new QTableWidgetItem(QStringLiteral("x")));
            }
        }
    }

private:
    QPropertyAnimation *m_anim = nullptr;
    QTimer *m_heavy = nullptr;
    QTableWidget *m_probe = nullptr;
    int m_h = 1;
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const bool heavy = (argc > 1) && (QString::fromLatin1(argv[1]) == QLatin1String("heavy"));
    std::printf("mode: %s\n", heavy ? "with a 1s table rebuild during the tween"
                                     : "tween alone, nothing else running");

    auto *win = new QWidget;
    win->resize(600, 500);
    auto *layout = new QVBoxLayout(win);
    auto *pad = new QWidget(win);
    pad->setMinimumHeight(200);
    layout->addWidget(pad);

    auto *t = new Tweened(heavy, win);
    layout->addWidget(t);
    layout->addStretch(1);

    win->show();

    struct Sample { qint64 ms; int h; };
    std::vector<Sample> samples;
    QElapsedTimer clock;

    auto *sampler = new QTimer(win);
    sampler->setTimerType(Qt::PreciseTimer);
    sampler->setInterval(2);
    QObject::connect(sampler, &QTimer::timeout, win, [&] {
        samples.push_back({clock.elapsed(), t->height()});
    });

    QObject::connect(t, &Tweened::destroyed, win, [] {});
    QTimer::singleShot(100, win, [&] {
        clock.start();
        sampler->start();
        t->reveal();
    });

    QTimer::singleShot(900, win, [&] {
        sampler->stop();

        int prev = -1, maxJump = 0;
        int distinct = 0;
        int lastH = -1;
        qint64 prevMs = -1;
        qint64 maxGap = 0;
        for (const Sample &s : samples) {
            if (prev >= 0)
                maxJump = qMax(maxJump, qAbs(s.h - prev));
            prev = s.h;
            if (s.h != lastH) {
                ++distinct;
                lastH = s.h;
            }
            if (prevMs >= 0)
                maxGap = qMax(maxGap, s.ms - prevMs);
            prevMs = s.ms;
        }

        std::printf("samples            : %zu\n", samples.size());
        std::printf("distinct heights   : %d\n", distinct);
        std::printf("largest jump       : %d px\n", maxJump);
        std::printf("largest sample gap : %lld ms\n", static_cast<long long>(maxGap));
        std::printf("verdict            : %s\n",
                    maxJump <= 20 ? "smooth" : "DROPPED FRAME");
        win->close();
    });

    return app.exec();
}

#include "measure_anim2.moc"
