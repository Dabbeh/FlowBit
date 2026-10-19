// Proves the Show Less crash is caused by re-parenting destroying the graphics
// effect that the collapse animation targets.
//
// Evidence: Flowbit.exe dies with 0xc0000005 inside Qt6Core.dll at a constant
// fault offset when Show Less is pressed. Two earlier fixes (deferring the
// monitor stop, adding explicit animation endpoints) addressed real defects but
// did not stop the crash, so neither was the cause.
//
// The actual mechanism:
//
//   * QWidget::setParent() DELETES the widget's QGraphicsEffect.
//   * AppActivityPanel owns a QGraphicsOpacityEffect and animates its "opacity".
//   * ResponsivePair::takeMiddle()/embedMiddle() call setParent() on the panel
//     whenever the window crosses the responsive layout breakpoint - which is
//     exactly what happens while the panel is being collapsed.
//
// After that the animation holds a pointer to a deleted object and the next
// animation step crashes.
//
// Usage: repro_reparent.exe [buggy|fixed]

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstring>

namespace {

// Mirrors ResponsivePair: moves the panel between two parents.
class Pair : public QWidget
{
public:
    Pair(QWidget *parent = nullptr) : QWidget(parent)
    {
        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_left = new QWidget(this);
        m_right = new QWidget(this);
        m_layout->addWidget(m_left);
        m_layout->addWidget(m_right);
    }

    // Same body as the application's helper.
    void embedMiddle(QWidget *w)
    {
        if (!m_layout || !w)
            return;
        if (w->parent() != this)
            w->setParent(this);
        m_layout->insertWidget(1, w);
    }

    void takeMiddle(QWidget *w)
    {
        if (!m_layout || !w)
            return;
        m_layout->removeWidget(w);
        w->setParent(nullptr);
    }

private:
    QHBoxLayout *m_layout = nullptr;
    QWidget *m_left = nullptr;
    QWidget *m_right = nullptr;
};

// Mirrors AppActivityPanel's animation setup and its collapse path.
class Panel : public QWidget
{
public:
    Panel(bool fixed, QWidget *parent = nullptr) : QWidget(parent), m_fixed(fixed)
    {
        setMinimumHeight(40);

        m_group = new QParallelAnimationGroup(this);

        auto *height = new QPropertyAnimation(this, "maximumHeight", m_group);
        height->setDuration(280);

        m_fx = new QGraphicsOpacityEffect(this);
        m_fx->setOpacity(0.0);
        setGraphicsEffect(m_fx);

        auto *fade = new QPropertyAnimation(m_fx, "opacity", m_group);
        fade->setDuration(280);

        m_group->addAnimation(height);
        m_group->addAnimation(fade);

        connect(m_group, &QVariantAnimation::finished, this, [this] {
            if (!m_expanded) {
                setVisible(false);
                setMaximumHeight(QWIDGETSIZE_MAX);
                if (m_fx)
                    m_fx->setOpacity(0.0);
            }
        });
    }

    void setExpanded(bool expanded)
    {
        // The fix under test: notice the effect was destroyed by a re-parent and
        // rebuild it before the animation touches it again.
        if (m_fixed && m_fx && graphicsEffect() != m_fx) {
            m_group->stop();
            m_fx = new QGraphicsOpacityEffect(this);
            m_fx->setOpacity(m_expanded ? 1.0 : 0.0);
            setGraphicsEffect(m_fx);
            if (auto *f = qobject_cast<QPropertyAnimation *>(m_group->animationAt(1))) {
                f->setTargetObject(m_fx);
                f->setPropertyName("opacity");
                f->setStartValue(0.0);
                f->setEndValue(1.0);
            }
        }

        m_expanded = expanded;
        if (expanded) {
            setVisible(true);
            setMaximumHeight(1);
            if (m_fx)
                m_fx->setOpacity(0.0);
            m_group->setDirection(QAbstractAnimation::Forward);
            m_group->stop();
            if (auto *h = qobject_cast<QPropertyAnimation *>(m_group->animationAt(0))) {
                h->setStartValue(1);
                h->setEndValue(160);
            }
            if (auto *f = qobject_cast<QPropertyAnimation *>(m_group->animationAt(1))) {
                f->setStartValue(0.0);
                f->setEndValue(1.0);
            }
            m_group->start();
        } else {
            m_group->setDirection(QAbstractAnimation::Backward);
            m_group->stop();
            m_group->start();
        }
    }

private:
    bool m_fixed = false;
    bool m_expanded = false;
    QParallelAnimationGroup *m_group = nullptr;
    QGraphicsOpacityEffect *m_fx = nullptr;
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const bool fixed = (argc > 1) && (std::strcmp(argv[1], "fixed") == 0);
    std::printf("mode: %s\n", fixed ? "fixed (rebuild the effect after re-parenting)"
                                    : "buggy (animation keeps the deleted effect)");

    auto *win = new QWidget;
    win->resize(900, 400);
    auto *outer = new QVBoxLayout(win);

    auto *pair = new Pair(win);
    outer->addWidget(pair);

    auto *panel = new Panel(fixed, pair);
    pair->embedMiddle(panel);
    outer->addStretch(1);
    win->show();

    // Collapse, and re-parent at the same time - which is what happens when the
    // window crosses the responsive breakpoint while the panel is animating.
    int step = 0;
    for (int i = 0; i < 30; ++i) {
        QTimer::singleShot(200 + i * 120, win, [pair, panel, &step] {
            if (step % 3 == 0) {
                pair->takeMiddle(panel);          // destroys the graphics effect
            } else if (step % 3 == 1) {
                pair->embedMiddle(panel);
            } else {
                panel->setExpanded(step % 2 == 0);   // animates the opacity
            }
            ++step;
        });
    }

    QTimer::singleShot(200 + 30 * 120 + 500, win, [win] {
        std::printf("SURVIVED all steps\n");
        win->close();
    });

    return app.exec();
}
