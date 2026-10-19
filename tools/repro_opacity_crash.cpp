// Reproduces the Show Less crash, with and without the fix.
//
// Evidence from the Windows event log: Flowbit.exe dies with an access violation
// (0xc0000005) inside Qt6Core.dll at a CONSTANT fault offset on collapse.
//
// The defect this models: the activity panel animates maximumHeight AND an
// opacity effect in one QParallelAnimationGroup, but only ever set start/end
// values on the height animation. Qt therefore warns
//
//     starting an animation without start value
//     starting an animation without end value
//
// for the opacity half on every toggle, and interpolates from a stale value.
// On collapse the widget is also hidden, so the animation is left addressing a
// property of a hidden (and in the app's case, re-parented) widget.
//
// Run with "buggy" or "fixed" as the first argument.

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstring>

namespace {

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
        m_expanded = expanded;
        if (expanded) {
            setVisible(true);
            setMaximumHeight(1);
            m_fx->setOpacity(0.0);
            m_group->setDirection(QAbstractAnimation::Forward);
            m_group->stop();

            if (auto *h = qobject_cast<QPropertyAnimation *>(m_group->animationAt(0))) {
                h->setStartValue(1);
                h->setEndValue(160);
            }
            // The fix: the opacity animation also gets explicit endpoints.
            if (m_fixed) {
                if (auto *f = qobject_cast<QPropertyAnimation *>(m_group->animationAt(1))) {
                    f->setStartValue(0.0);
                    f->setEndValue(1.0);
                }
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
    std::printf("mode: %s\n", fixed ? "fixed" : "buggy (no endpoints on the opacity animation)");

    auto *win = new QWidget;
    win->resize(400, 300);
    auto *layout = new QVBoxLayout(win);

    auto *panel = new Panel(fixed, win);
    layout->addWidget(panel);
    win->show();

    // Many rapid toggles, including ones that interrupt an animation already in
    // flight - exactly what clicking Show More / Show Less quickly produces.
    int cycles = 0;
    for (int i = 0; i < 60; ++i) {
        QTimer::singleShot(150 + i * 60, win, [panel, &cycles] {
            panel->setExpanded(cycles % 2 == 0);
            ++cycles;
        });
    }

    QTimer::singleShot(150 + 60 * 60 + 500, win, [win] {
        std::printf("SURVIVED all 60 toggles\n");
        win->close();
    });

    return app.exec();
}
