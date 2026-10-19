#pragma once

#include <QIcon>
#include <QString>

namespace net {

// Renders the Flowbit brand mark from its vector source.
//
// The logo is two flowing ribbon paths defined in the SVGs under docs/. Those
// files are the single source of truth for the artwork - the tray glyph, the
// window icon and every PNG/ICO in the size set are all derived from them, so
// the shape cannot drift between the packaged icon and what the app draws.
//
// An earlier version hand-painted an approximation of the mark with QPainter
// primitives; it produced a conventional letter F (rectangular stem plus two
// bars) rather than the flowing ribbons of the real logo.
namespace brand {

// Absolute path or Qt resource reference to the logo to use. When empty, the
// renderer falls back to the icon embedded in the executable's resources, and
// then to a painted approximation so the app never shows a blank tray icon.
void setSvgPath(const QString &path);

// The mark at `size` px, with the plate as designed (navy for dark, white for
// light). `slash` draws a red diagonal across it, used to signal "limiting off".
// `plateForDarkTheme` selects which of the two supplied variants to draw.
QIcon mark(int size = 128, bool slash = false, bool plateForDarkTheme = true);

// True when the vector source was found and loaded. False means the painted
// fallback is in use, which the UI surfaces as a warning.
bool usingVectorSource();

} // namespace brand
} // namespace net
