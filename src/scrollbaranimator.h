#pragma once

#include <QObject>
#include <QPointer>

class QAbstractScrollArea;
class QPropertyAnimation;
class QScrollBar;
class QWidget;

namespace net {

// A smoother scrollbar: it fades in when the pointer enters the scroll area, and fades out
// when it leaves.
//
// WHY THIS NEEDS CODE RATHER THAN A STYLESHEET
//
// Qt style sheets are static. There is no transition property and no way to animate a rule
// from the sheet itself, so anything that changes over time has to be driven by an animation
// on a widget property. This installs an event filter on the scroll area, watches for enter
// and leave, and animates the handle's opacity.
//
// WHY AN EVENT FILTER RATHER THAN A SUBCLASS
//
// The scroll areas in this application are created by several different builders, and every
// one of them would need changing to use a subclass. Filtering on the existing widgets means
// one call per scroll area and no changes to the widgets themselves.
//
// COST
//
// A QGraphicsOpacityEffect renders its widget into an offscreen buffer, which this project has
// already found to be expensive for a large subtree. A scrollbar handle is a handful of pixels,
// so the cost here is negligible - but the effect is only attached while an animation is
// actually running, and removed when it settles, so an idle window pays nothing at all.
class ScrollBarAnimator : public QObject
{
    Q_OBJECT

public:
    explicit ScrollBarAnimator(QAbstractScrollArea *area);

    // Attaches one to every scroll area inside `root`, including `root` itself if it is one.
    //
    // Stops at a scroll area that already has one, so calling this twice cannot stack filters
    // and double the animation rate.
    static void install(QWidget *root);

    // How long the fade takes, in milliseconds. Short enough to feel immediate, long enough
    // not to flash.
    static constexpr int kFadeMs = 160;

    // Whether the scroll area fades its handle on hover.
    //
    // Off means the handle is always at full opacity, which is what a touch or keyboard user
    // wants: there is no pointer to enter the area, so a fade-on-hover handle would never
    // appear for them.
    void setFadeEnabled(bool enabled);
    bool fadeEnabled() const { return m_fadeEnabled; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Animates the handle's opacity towards `to`, attaching the effect first when needed.
    void fadeTo(qreal to);

    // Removes the effect once it is fully transparent, so an idle or untouched scroll area
    // costs nothing.
    void settle();

    QPointer<QAbstractScrollArea> m_area;
    QPointer<QScrollBar> m_bar;
    QPropertyAnimation *m_anim = nullptr;
    bool m_fadeEnabled = true;
    bool m_hovered = false;
};

} // namespace net
