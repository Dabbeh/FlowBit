#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace net {

// Vector icons drawn with QPainter.
//
// Why not Unicode glyphs: the sidebar used characters such as U+25D4, U+2261,
// U+25F4, U+25A6, U+25A4 and U+2699. Those have no guaranteed shape - Windows
// substitutes whichever font claims the codepoint, so the set rendered at
// mismatched weights, sizes and optical centres, and one of them (the gear) even
// came through a colour-emoji font. That is the "icons look wrong / inconsistent"
// report, and it cannot be fixed by choosing different characters: the problem is
// that a text glyph is not an icon.
//
// These are drawn to a fixed 24x24 grid with one stroke width and one visual
// weight, then rendered at whatever size is asked for, so they stay consistent at
// any DPI and can be tinted to match the theme.
namespace icons {

enum class Kind {
    Dashboard,    // gauge / live reading
    Limiter,      // slider track with a handle
    Scheduler,    // clock
    AppLimiter,   // window with a bar
    Statistics,   // bar chart
    Settings,     // gear
};

// Paints `kind` into `rect` using `colour`, at the grid resolution of the rect.
void paint(QPainter &painter, Kind kind, const QRectF &rect, const QColor &colour);

// Convenience: a pixmap at the given device pixel ratio, for a QLabel or QIcon.
QPixmap pixmap(Kind kind, int size, const QColor &colour, qreal dpr = 1.0);

QIcon icon(Kind kind, const QColor &colour);

} // namespace icons
} // namespace net
