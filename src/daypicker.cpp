#include "daypicker.h"

#include "theme.h"
#include "units.h"

#include <QMouseEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>

namespace net {

namespace {
// Chip metrics.
//
// These were previously 23x18 with a 7px font and only two letters of the day
// name ("Mo", "Tu"), which read as cramped and inconsistent at any normal dialog
// width. The size is now derived from the font that will actually be used, so the
// chips scale with the UI font and the labels always fit rather than clipping.
constexpr int kGap = 4;
constexpr int kMargin = 2;

constexpr int kMinChipW = 30;
constexpr int kChipH = 26;

// Single-letter day labels, but at a readable size and with the full name in the
// tooltip. Two letters in a 23px chip was the cramped look being complained about;
// one letter in a properly sized chip is clearer, and "F" for Friday is
// unambiguous against "T"/"S" being the only repeats.
const char *kShortDay[7] = {"M", "T", "W", "T", "F", "S", "S"};
const char *kLongDay[7] = {"Monday", "Tuesday", "Wednesday", "Thursday",
                           "Friday", "Saturday", "Sunday"};
} // namespace

DayPicker::DayPicker(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Days this rule applies to (click a day to toggle)"));
    setMouseTracking(true);
}

void DayPicker::setReadOnly(bool readOnly)
{
    if (m_readOnly == readOnly)
        return;
    m_readOnly = readOnly;
    setCursor(readOnly ? Qt::ArrowCursor : Qt::PointingHandCursor);
    setMouseTracking(!readOnly);
    setToolTip(readOnly
                   ? QStringLiteral("Days this schedule runs. Use Edit to change them.")
                   : QStringLiteral("Days this rule applies to (click a day to toggle)"));
    if (readOnly && m_hover != -1)
        m_hover = -1;
    update();
}

void DayPicker::setMask(quint8 mask)
{
    // Never allow an empty mask: a rule that can never match is almost certainly
    // a mistake, and it would silently disable the row.
    if (mask == 0)
        mask = 0x7F;
    if (m_mask == mask)
        return;
    m_mask = mask;
    update();
    emit maskChanged(m_mask);
}

QSize DayPicker::sizeHint() const
{
    const int w = kMargin * 2 + 7 * chipWidth() + 6 * kGap;
    return QSize(w, kMargin * 2 + kChipH);
}

int DayPicker::chipWidth() const
{
    // Wide enough for the widest single letter at the chip font, so the size
    // follows the UI font instead of a hard-coded number that clips at larger
    // font settings.
    const QFontMetrics fm(theme::uiFont(9, true));
    return qMax(kMinChipW, fm.horizontalAdvance(QLatin1String("W")) + 18);
}

QRectF DayPicker::chipRect(int index) const
{
    const qreal x = kMargin + index * (chipWidth() + kGap);
    return QRectF(x, kMargin, chipWidth(), kChipH);
}

int DayPicker::chipAt(const QPoint &pos) const
{
    for (int i = 0; i < 7; ++i) {
        if (chipRect(i).contains(pos))
            return i;
    }
    return -1;
}

void DayPicker::toggle(int index)
{
    if (index < 0 || index > 6)
        return;

    const quint8 bit = quint8(1u << index);
    quint8 next = m_mask ^ bit;
    if (next == 0)
        next = 0x7F;  // refuse to leave nothing selected
    setMask(next);
}

void DayPicker::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setFont(theme::uiFont(9, true));

    for (int i = 0; i < 7; ++i) {
        const QRectF r = chipRect(i);
        const bool on = (m_mask >> i) & 0x1;

        // Selected chips use a light neutral fill rather than an accent colour,
        // so the schedule surface stays monochrome. Selection is carried by
        // contrast (light fill, dark label) instead of by hue.
        QColor fill;
        QColor edge;
        QColor text;
        if (on) {
            fill = theme::kTextSecondary();
            edge = theme::kTextSecondary();
            text = theme::kPanel();
        } else {
            fill = (i == m_hover) ? theme::kPanelRaised() : theme::kPanelSunken();
            edge = theme::kBorder();
            text = theme::kTextMuted();
        }

        p.setPen(QPen(edge, 1));
        p.setBrush(fill);
        p.drawRoundedRect(r, 6, 6);

        p.setPen(text);
        p.drawText(r, Qt::AlignCenter, QString::fromLatin1(kShortDay[i]));
    }
}

bool DayPicker::event(QEvent *event)
{
    // Per-chip tooltips: the chip shows one letter for legibility, so the full
    // day name must be discoverable on hover.
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int index = chipAt(help->pos());
        if (index >= 0 && !m_readOnly) {
            const bool on = (m_mask >> index) & 0x1;
            QToolTip::showText(help->globalPos(),
                               QStringLiteral("%1 - click to %2")
                                   .arg(QString::fromLatin1(kLongDay[index]),
                                        on ? QStringLiteral("exclude")
                                           : QStringLiteral("include")),
                               this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(event);
}

void DayPicker::mousePressEvent(QMouseEvent *event)
{
    // A display-only picker ignores clicks: a saved rule must not be altered by
    // an accidental click on its day chips.
    if (m_readOnly || event->button() != Qt::LeftButton)
        return;
    toggle(chipAt(event->pos()));
}

void DayPicker::mouseMoveEvent(QMouseEvent *event)
{
    if (m_readOnly)
        return;
    const int hover = chipAt(event->pos());
    if (hover != m_hover) {
        m_hover = hover;
        update();
    }
}

void DayPicker::leaveEvent(QEvent *)
{
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
}

} // namespace net
