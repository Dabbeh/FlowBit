#pragma once

#include <QColor>
#include <QFont>
#include <QString>

class QWidget;

namespace net {
namespace theme {

// ---------------------------------------------------------------------------
// Themes
//
// Two neutrally-toned themes, plus a "follow the OS" mode:
//
//   Dark  - near-black neutral greys. No colour cast in the chrome, so the only
//           saturated things on screen are the two directional colours.
//   Light - white cards on a very light grey window, dark text.
//
// Cyan still reads as incoming traffic and orange as outgoing in BOTH themes.
// That mapping is deliberate and load-bearing: recolouring upload to match the
// chrome would make the two directions indistinguishable at a glance. The
// accents are darkened in light mode so they keep contrast on white.
// ---------------------------------------------------------------------------

enum class Mode {
    Auto,   // follow the Windows apps-light/dark setting, live
    Light,
    Dark,
};

struct Palette {
    // Surfaces, dark -> light within the theme
    QColor background;     // window behind the cards
    QColor sidebar;        // navigation rail
    QColor panel;          // card / plate
    QColor panelRaised;    // elevated surface (inputs, raised rows)
    QColor panelSunken;    // well areas, graph bed
    QColor hover;          // hover background
    QColor navActive;      // active navigation item
    QColor border;         // hairline separators
    QColor borderStrong;   // focus rings and emphasised outlines

    // Text
    QColor textPrimary;
    QColor textSecondary;
    QColor textMuted;

    // Monochrome interactive roles. The interface is black/white/grey only, so
    // "primary action" is a filled monochrome button, not a coloured one.
    QColor primaryButton;
    QColor primaryButtonText;
    QColor secondaryButton;
    QColor secondaryButtonText;

    // Toggle switch
    QColor toggleOnTrack;
    QColor toggleOffTrack;
    QColor toggleOnText;
    QColor toggleOffText;

    // Semantic status only. These are NOT part of the monochrome chrome: they
    // exist so an error, a warning and a healthy state stay distinguishable.
    // The spec's "no new accent colours" applies to decoration, not to status.
    QColor green;
    QColor red;
    QColor amber;
};

// ---------------------------------------------------------------------------
// Graph series colours - FROZEN.
//
// The traffic graph must keep the exact colours it has always used, in both
// themes. They are therefore defined as constants here rather than read from
// the active palette: the interface is monochrome, but the graph is not, and
// routing it through the palette would silently recolour it.
// ---------------------------------------------------------------------------
inline QColor kGraphDownload() { return QColor(0x22, 0xC7, 0xE0); }  // cyan
inline QColor kGraphUpload() { return QColor(0xFF, 0x95, 0x2E); }    // orange

// The palette for the theme currently in effect.
const Palette &palette();

    // Makes the native title bar follow the theme.
    //
    // Windows paints the caption itself and Qt's palette cannot reach it, so a dark theme left
    // a black bar over a charcoal window. Asks DWM for dark-mode respect (attribute 20, with a
    // fallback to 19 for older Windows 10), and for the exact caption and text colours where
    // the build supports it (Windows 11 22000+; the calls fail harmlessly elsewhere).
    //
    // Returns whether the exact caption colour was applied - false on older Windows builds,
    // where only the dark-mode flag exists. Call again after the window is shown and whenever
    // the theme changes.
    bool applyTitleBar(QWidget *window);

// Resolves Auto against the OS setting and installs the result, restyling the
// application. Call on startup and whenever the user changes the preference.
void setMode(Mode mode);
Mode mode();

// The theme actually in effect (Auto resolved).
bool isDark();

// ---------------------------------------------------------------------------
// Colour accessors
//
// These deliberately keep the historical `theme::kFoo` spelling working as
// functions, so the ~44 existing call sites did not have to change when the
// palette became runtime-switchable.
// ---------------------------------------------------------------------------

inline QColor kBackground() { return palette().background; }
inline QColor kSidebar() { return palette().sidebar; }
inline QColor kPanel() { return palette().panel; }
inline QColor kPanelRaised() { return palette().panelRaised; }
inline QColor kPanelSunken() { return palette().panelSunken; }
inline QColor kHover() { return palette().hover; }
inline QColor kNavActive() { return palette().navActive; }
inline QColor kBorder() { return palette().border; }
inline QColor kBorderStrong() { return palette().borderStrong; }

inline QColor kTextPrimary() { return palette().textPrimary; }
inline QColor kTextSecondary() { return palette().textSecondary; }
inline QColor kTextMuted() { return palette().textMuted; }

inline QColor kPrimaryButton() { return palette().primaryButton; }
inline QColor kPrimaryButtonText() { return palette().primaryButtonText; }
inline QColor kSecondaryButton() { return palette().secondaryButton; }
inline QColor kSecondaryButtonText() { return palette().secondaryButtonText; }

inline QColor kGreen() { return palette().green; }
inline QColor kRed() { return palette().red; }
inline QColor kAmber() { return palette().amber; }

inline QColor kToggleOnTrack() { return palette().toggleOnTrack; }
inline QColor kToggleOffTrack() { return palette().toggleOffTrack; }
inline QColor kToggleOnText() { return palette().toggleOnText; }
inline QColor kToggleOffText() { return palette().toggleOffText; }

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

// The UI face: geometric-ish sans that ships with Windows.
QString uiFontFamily();

// Big numeric readouts. Uses a tabular/monospaced face so digits do not jitter
// as the value changes every second.
QFont counterFont(int pointSize, bool bold = true);
QFont uiFont(int pointSize, bool bold = false);

// ---------------------------------------------------------------------------
// Shared styling
// ---------------------------------------------------------------------------

// True when the OS reports that apps should use the light theme.
bool systemPrefersLight();

// Applies the stylesheet for the current palette.
void applyApplicationTheme();

// Styles one card/panel frame (the top-level boxes in the grid).
QString cardStyleSheet(const QString &objectName = QString());

// Paints the standard card chrome (rounded rect + hairline border) for a widget
// that wants to draw its own background, e.g. the dashboard tiles.
void paintCard(QWidget *widget, bool raised = false);

// Formats a byte/second rate using the unit family the UI is configured for.
QString formatBytesPerSec(double bytesPerSec);

// Formats a value in kibibytes/sec (the historical storage unit) as a rate.
QString formatKibPerSec(qint64 kibPerSec);

} // namespace theme
} // namespace net
