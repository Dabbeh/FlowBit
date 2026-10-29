#include "theme.h"
#include "checkmark.h"

#include <QApplication>

#include <dwmapi.h>
#include <windows.h>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QWidget>

#include <windows.h>

namespace net {
namespace theme {

namespace {

// ---------------------------------------------------------------------------
// Dark - "Minimal Black".
//
// Near-black throughout rather than pure black: #0D0D0D for the window, with
// each layer a step lighter so elevation reads without shadows or glows. Flat
// by design - no gradients, no glows.
// ---------------------------------------------------------------------------
const Palette kDark = {
    /* background    */ QColor(0x0D, 0x0D, 0x0D),  // main background
    /* sidebar       */ QColor(0x11, 0x11, 0x11),
    /* panel         */ QColor(0x17, 0x17, 0x17),  // cards / panels
    /* panelRaised   */ QColor(0x20, 0x20, 0x20),  // elevated surfaces
    /* panelSunken   */ QColor(0x1C, 0x1C, 0x1C),  // input background / wells
    /* hover         */ QColor(0x26, 0x26, 0x26),
    /* navActive     */ QColor(0x2D, 0x2D, 0x2D),
    /* border        */ QColor(0x2A, 0x2A, 0x2A),
    /* borderStrong  */ QColor(0x3A, 0x3A, 0x3A),

    /* textPrimary   */ QColor(0xF5, 0xF5, 0xF5),
    /* textSecondary */ QColor(0xA3, 0xA3, 0xA3),
    /* textMuted     */ QColor(0x73, 0x73, 0x73),

    // Monochrome actions: white fill, black label.
    /* primaryButton */ QColor(0xF5, 0xF5, 0xF5),
    /* primaryButtonText */ QColor(0x11, 0x11, 0x11),
    /* secondaryButton */ QColor(0x26, 0x26, 0x26),
    /* secondaryButtonText */ QColor(0xF5, 0xF5, 0xF5),

    /* toggleOnTrack */ QColor(0xF5, 0xF5, 0xF5),  // enabled: white
    /* toggleOffTrack*/ QColor(0x3A, 0x3A, 0x3A),  // disabled: gray
    /* toggleOnText  */ QColor(0x11, 0x11, 0x11),
    /* toggleOffText */ QColor(0xA3, 0xA3, 0xA3),

    // Status only. Kept chromatic so an error still reads as an error; these
    // are never used for decoration.
    /* green         */ QColor(0x4A, 0xDE, 0x82),
    /* red           */ QColor(0xF8, 0x71, 0x71),
    /* amber         */ QColor(0xFB, 0xBF, 0x24),
};

// ---------------------------------------------------------------------------
// Light - "Minimal White".
//
// Soft white surfaces with thin grey borders. Cards are pure white against a
// #FAFAFA page, separated by the border rather than by shadow.
// ---------------------------------------------------------------------------
const Palette kLight = {
    /* background    */ QColor(0xFA, 0xFA, 0xFA),  // main background
    /* sidebar       */ QColor(0xFF, 0xFF, 0xFF),
    /* panel         */ QColor(0xFF, 0xFF, 0xFF),  // cards / panels
    /* panelRaised   */ QColor(0xF5, 0xF5, 0xF5),  // elevated surfaces
    /* panelSunken   */ QColor(0xF7, 0xF7, 0xF7),  // input background / wells
    /* hover         */ QColor(0xF0, 0xF0, 0xF0),
    /* navActive     */ QColor(0xEE, 0xEE, 0xEE),
    /* border        */ QColor(0xE5, 0xE5, 0xE5),
    /* borderStrong  */ QColor(0xCF, 0xCF, 0xCF),

    /* textPrimary   */ QColor(0x17, 0x17, 0x17),
    /* textSecondary */ QColor(0x66, 0x66, 0x66),
    /* textMuted     */ QColor(0x8A, 0x8A, 0x8A),

    // Monochrome actions: black fill, white label.
    /* primaryButton */ QColor(0x17, 0x17, 0x17),
    /* primaryButtonText */ QColor(0xFF, 0xFF, 0xFF),
    /* secondaryButton */ QColor(0xF0, 0xF0, 0xF0),
    /* secondaryButtonText */ QColor(0x17, 0x17, 0x17),

    /* toggleOnTrack */ QColor(0x17, 0x17, 0x17),  // enabled: black
    /* toggleOffTrack*/ QColor(0xD4, 0xD4, 0xD4),  // disabled: light gray
    /* toggleOnText  */ QColor(0xFF, 0xFF, 0xFF),
    /* toggleOffText */ QColor(0x66, 0x66, 0x66),

    /* green         */ QColor(0x16, 0xA3, 0x4A),
    /* red           */ QColor(0xDC, 0x26, 0x26),
    /* amber         */ QColor(0xB4, 0x54, 0x09),
};

Mode g_mode = Mode::Auto;
bool g_dark = true;

QString pickFirstAvailable(const QStringList &candidates, const QString &fallback)
{
    const QStringList families = QFontDatabase::families();
    for (const QString &name : candidates) {
        if (families.contains(name, Qt::CaseInsensitive))
            return name;
    }
    return fallback;
}

// Reads the Windows "apps use light theme" preference straight from the
// registry. The documented AppsUseLightTheme value is 0 for dark, 1 for light.
bool readSystemPrefersLight()
{
    QSettings reg(QStringLiteral(
                      "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                  QSettings::NativeFormat);
    const QVariant v = reg.value(QStringLiteral("AppsUseLightTheme"));
    if (!v.isValid())
        return false;   // absent => older build => dark is the Windows default
    return v.toInt() != 0;
}

} // namespace

bool systemPrefersLight()
{
    return readSystemPrefersLight();
}

const Palette &palette()
{
    return g_dark ? kDark : kLight;
}

Mode mode()
{
    return g_mode;
}

bool isDark()
{
    return g_dark;
}

void setMode(Mode mode)
{
    g_mode = mode;
    g_dark = (mode == Mode::Dark) || (mode == Mode::Auto && !readSystemPrefersLight());
    applyApplicationTheme();
}

QString uiFontFamily()
{
    // Segoe UI Variable ships with Windows 11; plain Segoe UI covers Windows 10.
    static const QString family = pickFirstAvailable(
        {QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI"),
         QStringLiteral("Inter"), QStringLiteral("Arial")},
        QStringLiteral("Sans Serif"));
    return family;
}

QFont uiFont(int pointSize, bool bold)
{
    QFont f(uiFontFamily(), pointSize);
    f.setBold(bold);
    return f;
}

QFont counterFont(int pointSize, bool bold)
{
    // Cascadia Mono / Consolas both have tabular digits, so a rate ticking from
    // 999 to 1000 KB/s does not shift the layout every second.
    static const QString family = pickFirstAvailable(
        {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
         QStringLiteral("Lucida Console"), QStringLiteral("Courier New")},
        QStringLiteral("Monospace"));
    QFont f(family, pointSize);
    f.setBold(bold);
    f.setStyleHint(QFont::Monospace);
    return f;
}

QString cardStyleSheet(const QString &objectName)
{
    const QString selector = objectName.isEmpty()
                                 ? QStringLiteral("QFrame#card")
                                 : QStringLiteral("QFrame#%1").arg(objectName);

    // The border alone is not enough in light mode: white cards on a pale grey
    // page read as one flat sheet. Qt's stylesheet shadows are unreliable on
    // Windows, so the lift comes from a slightly stronger border plus a darker
    // ring, which holds up at any DPI.
    return QStringLiteral(
               "%1 {"
               "  background-color: %2;"
               "  border: 1px solid %3;"
               "  border-radius: 10px;"
               "}")
        .arg(selector, kPanel().name(), kBorder().name());
}

void paintCard(QWidget *widget, bool raised)
{
    if (!widget)
        return;

    QPainter p(widget);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(kBorder(), 1));
    p.setBrush(raised ? kPanelRaised() : kPanel());

    const QRectF r = QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.drawRoundedRect(r, 10, 10);
}

void applyApplicationTheme()
{
    QApplication::setFont(uiFont(9));

    // Palette so native widgets (menus, tooltips, spin boxes) follow the theme.
    QPalette pal;
    pal.setColor(QPalette::Window, kBackground());
    pal.setColor(QPalette::WindowText, kTextPrimary());
    pal.setColor(QPalette::Base, kPanelSunken());
    pal.setColor(QPalette::AlternateBase, kPanel());
    pal.setColor(QPalette::Text, kTextPrimary());
    pal.setColor(QPalette::Button, kPanelRaised());
    pal.setColor(QPalette::ButtonText, kTextPrimary());
    // Monochrome selection: the chrome never introduces colour, so highlight is
    // the primary ink colour rather than an accent.
    pal.setColor(QPalette::Highlight, kPrimaryButton());
    pal.setColor(QPalette::HighlightedText, kPrimaryButtonText());
    pal.setColor(QPalette::ToolTipBase, kPanelRaised());
    pal.setColor(QPalette::ToolTipText, kTextPrimary());
    pal.setColor(QPalette::PlaceholderText, kTextMuted());
    pal.setColor(QPalette::Disabled, QPalette::Text, kTextMuted());
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, kTextMuted());
    qApp->setPalette(pal);

    // One flat, monochrome design system. No gradients, no glows, no coloured
    // outlines anywhere in the chrome - emphasis comes from value (light on
    // dark, dark on light) and from hairline borders.
    // The tick, written once per run. An empty path means it could not be written, and the
    // indicator then falls back to a plain fill - which is why this is not fatal.
    const QString generatedCheckmark = generateCheckmark();

    const QString sheet = QStringLiteral(R"(
QWidget {
    color: %(text)s;
    font-family: "%(family)s";
}
QMainWindow, QDialog { background-color: %(bg)s; }

/* ---- cards and panels ------------------------------------------------ */
QFrame#card {
    background-color: %(panel)s;
    border: 1px solid %(border)s;
    border-radius: 11px;
}
QFrame#ruleRow {
    background-color: %(raised)s;
    border: 1px solid %(border)s;
    border-radius: 10px;
}

/* ---- sidebar --------------------------------------------------------- */
/* A flat rail. The active item is marked by background value and a brighter
   label - no coloured indicator, per the monochrome design. */
QWidget#sidebar {
    background-color: %(sidebar)s;
    border: 0;
}
QFrame#sidebarDivider {
    background-color: %(border)s;
    border: 0;
    max-width: 1px;
}
QPushButton#navItem {
    background-color: transparent;
    color: %(muted)s;
    border: 0;
    border-radius: 8px;
    /* Roomy enough that the glyph reads as an icon rather than as a stray
       character, and so the row does not look cramped against the rail edge. */
    padding: 9px 12px;
    text-align: left;
    font-weight: 400;
    font-size: 10pt;
}
QPushButton#navItem:hover {
    background-color: %(hover)s;
    color: %(text)s;
}
QPushButton#navItem:checked {
    background-color: %(navActive)s;
    color: %(text)s;
    font-weight: 600;
}

/* ---- pages ----------------------------------------------------------- */
QLabel#pageTitle {
    color: %(text)s;
    font-size: 15pt;
    font-weight: 600;
}
QLabel#fieldLabel {
    color: %(secondary)s;
    font-size: 8pt;
    font-weight: 600;
}
QLabel#statValue {
    color: %(text)s;
    font-size: 13pt;
    font-weight: 600;
}

/* ---- schedule cards -------------------------------------------------- */
QFrame#scheduleCard {
    background-color: %(raised)s;
    border: 1px solid %(border)s;
    border-radius: 10px;
}
/* The schedule currently applying is marked with a thicker left edge rather
   than a colour, so it stays monochrome in both themes. */
QFrame#scheduleCard[active="true"] {
    border-left: 3px solid %(text)s;
}
QLabel#scheduleName {
    font-weight: 600;
}
QLabel#scheduleTime {
    color: %(secondary)s;
    font-size: 8pt;
}
QLabel#cardGlyph {
    color: %(muted)s;
    font-size: 14pt;
}
QLabel#limitValue {
    color: %(text)s;
}
QPushButton#scheduleToggle {
    background-color: transparent;
    color: %(muted)s;
    border: 1px solid %(border)s;
    border-radius: 12px;
    padding: 3px 12px;
    font-weight: 400;
}
QPushButton#scheduleToggle:hover { border-color: %(strong)s; }
QPushButton#scheduleToggle:checked {
    background-color: %(navActive)s;
    color: %(text)s;
    border-color: %(strong)s;
    font-weight: 600;
}
QLabel#cardTitle {
    color: %(muted)s;
    font-size: 8pt;
    font-weight: 600;
    letter-spacing: 1px;
}

/* ---- text roles ------------------------------------------------------ */
QLabel#hint   { color: %(muted)s; }
QLabel#sub, QLabel#usageProjection { color: %(secondary)s; font-size: 8pt; }
QLabel#error  { color: %(red)s; font-weight: 600; }

/* The strip the error sits in.
   Given a surface of its own, so the dismiss cross has a corner to occupy. The left edge is
   the accent: it marks the strip as an alert without tinting an area large enough to fight the
   red text on top of it. */
QWidget#errorStrip {
    background-color: %(panel)s;
    border: 1px solid %(border)s;
    border-left: 3px solid %(red)s;
    border-radius: 8px;
}

/* ---- inputs ---------------------------------------------------------- */
QSpinBox, QDoubleSpinBox, QLineEdit, QComboBox, QTimeEdit {
    background-color: %(input)s;
    border: 1px solid %(border)s;
    border-radius: 7px;
    padding: 4px 7px;
    selection-background-color: %(primary)s;
    selection-color: %(primaryText)s;
}
/* Focus is a heavier monochrome outline, never a coloured ring. */
QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus,
QComboBox:focus, QTimeEdit:focus {
    border: 1px solid %(strong)s;
}
QSpinBox:disabled, QDoubleSpinBox:disabled, QLineEdit:disabled,
QComboBox:disabled {
    color: %(muted)s;
    background-color: %(raised)s;
}
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 14px; }
QComboBox::drop-down { border: 0; width: 18px; }
QComboBox QAbstractItemView {
    background-color: %(panel)s;
    border: 1px solid %(border)s;
    selection-background-color: %(navActive)s;
    selection-color: %(text)s;
    outline: 0;
}

/* ---- buttons --------------------------------------------------------- */
/* Filled monochrome primary. Buttons without an object name are primary by
   default, which is how the existing call sites expect them to look. */
QPushButton {
    background-color: %(primary)s;
    color: %(primaryText)s;
    border: 1px solid %(primary)s;
    border-radius: 7px;
    padding: 5px 13px;
    font-weight: 600;
}
QPushButton:hover   { background-color: %(primaryHover)s; border-color: %(primaryHover)s; }
QPushButton:pressed { background-color: %(secondaryBtn)s; border-color: %(strong)s; }
QPushButton:disabled {
    background-color: %(raised)s;
    border-color: %(border)s;
    color: %(muted)s;
}

QPushButton#ghost {
    background: transparent;
    color: %(text)s;
    border: 1px solid %(border)s;
    font-weight: 400;
}
QPushButton#ghost:hover   { background-color: %(hover)s; border-color: %(strong)s; }
QPushButton#ghost:pressed { background-color: %(navActive)s; }

/* The dismiss cross on the error strip.
   Borderless, so it reads as part of the message rather than as a button dropped onto it. The
   hover fill is the only chrome, which keeps the resting state quiet and still makes it
   obviously clickable. */
QPushButton#errorDismiss {
    background: transparent;
    border: 0;
    padding: 0;
}
QPushButton#errorDismiss:hover   { background-color: %(hover)s; border-radius: 5px; }
QPushButton#errorDismiss:pressed { background-color: %(navActive)s; border-radius: 5px; }

/* Destructive actions read as outlined and adopt the error colour only on
   hover, so the resting interface stays monochrome. */
QPushButton#danger {
    background-color: %(secondaryBtn)s;
    color: %(text)s;
    border: 1px solid %(border)s;
    font-weight: 400;
}
QPushButton#danger:hover { color: %(red)s; border-color: %(red)s; }

QPushButton#preset {
    background-color: %(secondaryBtn)s;
    color: %(text)s;
    border: 1px solid %(border)s;
    border-radius: 14px;
    padding: 5px 14px;
    font-weight: 400;
}
QPushButton#preset:hover { background-color: %(hover)s; border-color: %(strong)s; }

/* ---- tables ---------------------------------------------------------- */
QTableWidget, QTableView {
    background-color: %(panel)s;
    alternate-background-color: %(sunken)s;
    border: 1px solid %(border)s;
    border-radius: 8px;
    gridline-color: %(border)s;
    outline: 0;
}
QTableWidget::item { padding: 3px 6px; }
QTableWidget::item:selected { background-color: %(navActive)s; color: %(text)s; }
/* The activity list is read-only, so hover is the only affordance it needs. */
QTableWidget::item:hover { background-color: %(hover)s; }
QHeaderView::section:hover { color: %(text)s; }
QHeaderView::section {
    background-color: %(panel)s;
    color: %(muted)s;
    border: 0;
    border-bottom: 1px solid %(border)s;
    padding: 4px 6px;
    font-size: 8pt;
    font-weight: 600;
}

/* ---- scroll areas ---------------------------------------------------- */
QScrollArea { background: transparent; border: 0; }
QScrollArea > QWidget > QWidget { background: transparent; }

/* ---- misc ------------------------------------------------------------ */
QGroupBox {
    border: 1px solid %(border)s;
    border-radius: 10px;
    margin-top: 10px;
    padding-top: 8px;
    background-color: %(panel)s;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
    color: %(muted)s;
}
/* ---- scroll bars ------------------------------------------------------ */
/*
 * Every sub-control is stated explicitly.
 *
 * Qt's own documentation is blunt about this: "if one property or sub-control is customized,
 * all the other properties or sub-controls must be customized as well". The previous rules
 * styled only the handle, so add-page and sub-page - the track above and below it - kept
 * their platform drawing. On this Qt build that renders as an unthemed block that does not
 * match the card it sits in, which is what made the bar look wrong.
 *
 * The geometry:
 *   - the bar is 12px with a 2px margin, so the handle clears the card edge rather than
 *     touching it;
 *   - the handle is 8px inside that, leaving room to widen on hover without moving;
 *   - the track is transparent, because a permanent rail competes with the content it
 *     scrolls. It becomes faintly visible on hover, which is when it is being aimed at.
 */
QScrollBar:vertical {
    background: transparent;
    width: 12px;
    margin: 2px 2px 2px 0;
    border: 0;
}
QScrollBar::handle:vertical {
    background: %(strong)s;
    border-radius: 4px;
    min-height: 32px;
    /* Inset from the bar's edges so the handle reads as a pill inside a gutter rather than
       filling it. Negative margins would be needed to widen on hover; a transparent border
       achieves the same without moving the geometry mid-animation. */
    margin: 0 2px;
}
QScrollBar::handle:vertical:hover { background: %(muted)s; }
QScrollBar::handle:vertical:pressed { background: %(text)s; }

/* The track either side of the handle. Transparent, but stated - see the note above. */
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

/* The arrows at each end. Height 0 removes them: the bar is dragged, not stepped, and the
   platform arrows are the part most likely to look out of place in a themed card. */
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0; width: 0; background: transparent; border: 0;
}

QScrollBar:horizontal {
    background: transparent;
    height: 12px;
    margin: 0 2px 2px 2px;
    border: 0;
}
QScrollBar::handle:horizontal {
    background: %(strong)s;
    border-radius: 4px;
    min-width: 32px;
    margin: 2px 0;
}
QScrollBar::handle:horizontal:hover { background: %(muted)s; }
QScrollBar::handle:horizontal:pressed { background: %(text)s; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    height: 0; width: 0; background: transparent; border: 0;
}

/* The corner where a vertical and horizontal bar would meet. Never visible in this
   application, because no view scrolls in both directions, but stated so it cannot fall back
   to platform drawing if one ever does. */
QScrollBar::corner { background: transparent; border: 0; }
QCheckBox::indicator {
    width: 18px; height: 18px;
    border: 1px solid %(muted)s;
    border-radius: 4px;
    background: %(input)s;
}
/*
 * Hover and focus lift the border rather than the fill, so an unselected box stays legible
 * against the card without competing with a selected one.
 */
QCheckBox::indicator:hover { border-color: %(text)s; }
QCheckBox::indicator:focus { border-color: %(text)s; }

/*
 * The checked state gets a tick, not only a fill.
 * 
 * A filled square alone was ambiguous: against a dark card an unchecked box and a checked one
 * are a small brightness difference, and the control gave no shape clue. An SVG tick drawn in
 * the background makes the two states unmistakable, which matters most for a control whose
 * whole job is to show whether something is on.
 */
QCheckBox::indicator:checked {
    background: %(primary)s;
    border-color: %(primary)s;
    image: url("%(checkmark)s");
}
QCheckBox::indicator:disabled {
    border-color: %(border)s;
    background: %(panel)s;
}
/*
 * A partially-set state, for a tri-state control if one is ever used.
 */
QCheckBox::indicator:indeterminate {
    background: %(input)s;
    border-color: %(primary)s;
}

/*
 * The label is part of the control, not a separate widget, so the text sits with the box
 * and inherits the pointer cursor. Spacing keeps it off the indicator's edge.
 */
QCheckBox { spacing: 8px; padding: 2px 0; }
QToolTip {
    background-color: %(raised)s;
    color: %(text)s;
    border: 1px solid %(border)s;
    padding: 4px 6px;
}
QStatusBar { color: %(muted)s; }
QMenu {
    background-color: %(panel)s;
    border: 1px solid %(border)s;
    border-radius: 8px;
    padding: 4px;
}
QMenu::item { padding: 5px 22px 5px 10px; border-radius: 5px; }
QMenu::item:selected { background-color: %(navActive)s; color: %(text)s; }
QMenu::separator { height: 1px; background: %(border)s; margin: 4px 6px; }
QSlider::groove:horizontal {
    height: 4px; border-radius: 2px; background: %(strong)s;
}
QSlider::sub-page:horizontal { background: %(primary)s; border-radius: 2px; }
QSlider::handle:horizontal {
    background: %(primary)s; border: 0; width: 14px; margin: -6px 0; border-radius: 7px;
}
)")
                             .replace(QStringLiteral("%(checkmark)s"), generatedCheckmark)
                             .replace(QStringLiteral("%(bg)s"), kBackground().name())
                             .replace(QStringLiteral("%(sidebar)s"), kSidebar().name())
                             .replace(QStringLiteral("%(panel)s"), kPanel().name())
                             .replace(QStringLiteral("%(raised)s"), kPanelRaised().name())
                             .replace(QStringLiteral("%(sunken)s"), kPanelSunken().name())
                             .replace(QStringLiteral("%(input)s"), kPanelSunken().name())
                             .replace(QStringLiteral("%(hover)s"), kHover().name())
                             .replace(QStringLiteral("%(navActive)s"), kNavActive().name())
                             .replace(QStringLiteral("%(border)s"), kBorder().name())
                             .replace(QStringLiteral("%(strong)s"), kBorderStrong().name())
                             .replace(QStringLiteral("%(text)s"), kTextPrimary().name())
                             .replace(QStringLiteral("%(secondary)s"), kTextSecondary().name())
                             .replace(QStringLiteral("%(muted)s"), kTextMuted().name())
                             .replace(QStringLiteral("%(red)s"), kRed().name())
                             .replace(QStringLiteral("%(primary)s"), kPrimaryButton().name())
                             .replace(QStringLiteral("%(primaryText)s"),
                                      kPrimaryButtonText().name())
                             .replace(QStringLiteral("%(secondaryBtn)s"), kSecondaryButton().name())
                             // Primary hover steps toward the text colour: on dark that
                             // darkens the white button, on light it lightens the black
                             // one, so the affordance reads correctly in both themes.
                             // Primary hover steps one increment toward the text
                             // colour: the white button darkens on the dark theme, the
                             // black button lightens on the light theme.
                             .replace(QStringLiteral("%(primaryHover)s"), kTextSecondary().name())
                             .replace(QStringLiteral("%(family)s"), uiFontFamily());

    qApp->setStyleSheet(sheet);
}
QString formatBytesPerSec(double bytesPerSec)
{
    static const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    double value = bytesPerSec > 0 ? bytesPerSec : 0.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    const int decimals = (unit == 0 || value >= 100.0) ? 0 : (value >= 10.0 ? 1 : 2);
    return QStringLiteral("%1 %2").arg(value, 0, 'f', decimals).arg(QLatin1String(units[unit]));
}

QString formatKibPerSec(qint64 kibPerSec)
{
    if (kibPerSec <= 0)
        return QStringLiteral("Uncapped");
    return formatBytesPerSec(double(kibPerSec) * 1024.0);
}

bool applyTitleBar(QWidget *window)
{
    // The native caption is drawn by DWM from its own state; a Qt palette never reaches it.
    // Each attribute is set independently and failures are counted rather than swallowed: the
    // dark-mode flag must land for the bar to be dark at all, the exact colours can only land
    // on Windows 11 22000+.
    if (!window)
        return false;

    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return false;

    const bool dark = isDark();

    // Attribute 20 everywhere it is supported; older Windows 10 builds only know 19.
    BOOL flag = dark ? TRUE : FALSE;
    if (DwmSetWindowAttribute(hwnd, 20, &flag, sizeof(flag)) != S_OK)
        DwmSetWindowAttribute(hwnd, 19, &flag, sizeof(flag));

    // The exact colours. COLORREF is 0x00BBGGRR, which is not hex-RGB's order - hence the
    // explicit channel picks rather than a hex round trip.
    const Palette pal = palette();
    const COLORREF caption = RGB(pal.background.red(), pal.background.green(), pal.background.blue());
    const COLORREF text = RGB(pal.textPrimary.red(), pal.textPrimary.green(), pal.textPrimary.blue());

    const HRESULT cap = DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));

    return cap == S_OK;
}

} // namespace theme

} // namespace net
