#pragma once

#include <QByteArray>
#include <QString>

namespace net {
namespace theme {

// The checkbox tick, generated at startup.
//
// A stylesheet can only point at an image file, and this project ships no .qrc, so the tick is
// drawn into an SVG and written somewhere Qt can load it from.
//
// WHY GENERATED RATHER THAN SHIPPED
//
// The tick has to contrast with the filled indicator. Shipped as a file, its colour would be
// baked in and would silently stop matching if the palette ever changed. Drawn from the
// palette's text colour, it is guaranteed to contrast by construction.
//
// WHY AN ABSOLUTE PATH
//
// Qt resolves url() in a stylesheet relative to the current working directory, not the
// executable's directory. A shortcut can set any working directory, so a relative path would
// work when launched one way and fail another. The file is written to the application's own
// data directory and referred to absolutely.
//
// Returns the path, or an empty string when it could not be written - in which case the
// stylesheet omits the tick and the indicator falls back to a plain fill, which is the
// behaviour before this existed rather than a broken image.
QString generateCheckmark();

// The tick's SVG contents, exposed so the stylesheet can inline it where a data URI is
// preferred over a file on disk.
QByteArray checkmarkSvg();

} // namespace theme
} // namespace net
