#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

class QWidget;

namespace net {

// A drawn cross, for dismiss buttons.
//
// WHY NOT A CHARACTER
//
// The dismiss button used the multiplication sign, U+2715. That glyph is not in Arial, which
// this application falls back to, so the button rendered as an empty outlined box with nothing
// visible inside it - which is exactly how it was reported: "just a fill in button".
//
// A glyph depends on the font having it. A drawn path does not, and it can be given a stroke
// weight that suits the size rather than whatever the font designer chose.
QPixmap crossPixmap(int size, const QColor &colour, qreal strokeWidth = 1.6, qreal scale = 0.42);

QIcon crossIcon(int size, const QColor &colour, qreal strokeWidth = 1.6, qreal scale = 0.42);

} // namespace net
