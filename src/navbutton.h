#pragma once

#include <QPushButton>

#include "navicons.h"

namespace net {

// A sidebar navigation button that draws its own icon.
//
// Why not QPushButton::setIcon(): with the application stylesheet applied
// (QPushButton#navItem has padding and text-align: left), Qt lays the icon out
// inside the button's content rect and, in this combination, gives it far less
// room than setIconSize() asks for. The result on screen was the icon cropped to a
// small fragment - a stubby arc, a short dash, a corner - even though the
// underlying pixmaps were correct (verified: every icon fills its own box).
//
// Painting the icon here removes that dependency entirely: the icon is drawn into
// a fixed 22px column at a position this class controls, and the text is drawn
// after it. It cannot be squeezed by the stylesheet.
class NavButton : public QPushButton
{
    Q_OBJECT

public:
    explicit NavButton(QWidget *parent = nullptr);

    void setIconKind(icons::Kind kind);
    icons::Kind iconKind() const { return m_kind; }

    // Whether the label is shown beside the icon (false in the collapsed rail).
    void setLabelVisible(bool visible);
    bool isLabelVisible() const { return m_labelVisible; }

    // The name shown when the label is hidden, and the full tooltip otherwise.
    void setLabelText(const QString &text);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    icons::Kind m_kind = icons::Kind::Dashboard;
    bool m_labelVisible = true;
    QString m_labelText;
};

} // namespace net
