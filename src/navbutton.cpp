#include "navbutton.h"

#include "theme.h"

#include <QFontMetrics>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>

namespace net {

namespace {
// The icon column. Fixed, so the icons line up in a clean vertical rail no matter
// how long the labels are or how the stylesheet sets padding.
constexpr int kIconColumn = 24;
constexpr int kIconSize = 20;      // the drawn size inside that column
constexpr int kLeftInset = 12;     // matches the stylesheet's left padding
constexpr int kTextGap = 6;
constexpr int kRowHeight = 38;
} // namespace

NavButton::NavButton(QWidget *parent) : QPushButton(parent)
{
    setObjectName(QStringLiteral("navItem"));
    setCheckable(true);
    setAutoDefault(false);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(kRowHeight);
    // No icon through Qt, and no text through Qt either: both are painted below,
    // which is what keeps the layout predictable.
    setIcon(QIcon());
    setText(QString());
}

void NavButton::setIconKind(icons::Kind kind)
{
    m_kind = kind;
    update();
}

void NavButton::setLabelVisible(bool visible)
{
    if (m_labelVisible == visible)
        return;
    m_labelVisible = visible;
    updateGeometry();
    update();
}

void NavButton::setLabelText(const QString &text)
{
    m_labelText = text;
    // The accessible name and tooltip still carry the label, so the collapsed rail
    // stays usable with a screen reader or a hover.
    setAccessibleName(text);
    updateGeometry();
    update();
}

QSize NavButton::sizeHint() const
{
    const QFontMetrics fm(font());
    const int textWidth = m_labelVisible ? (kTextGap + fm.horizontalAdvance(m_labelText)) : 0;
    return QSize(kLeftInset + kIconColumn + textWidth + kLeftInset, kRowHeight);
}

void NavButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QStyleOptionButton opt;
    opt.initFrom(this);

    // 1. Background, hover and checked states: let the stylesheet draw the frame
    //    only, by painting the button without its contents.
    QStyleOptionButton frame = opt;
    frame.text.clear();
    frame.icon = QIcon();
    style()->drawControl(QStyle::CE_PushButton, &frame, &p, this);

    // 2. The icon, into its own fixed column.
    const QRect iconRect(kLeftInset, (height() - kIconSize) / 2, kIconSize, kIconSize);
    const QColor tint = isChecked() ? theme::kTextPrimary() : theme::kTextMuted();
    icons::paint(p, m_kind, QRectF(iconRect), tint);

    // 3. The label, if the rail is expanded.
    if (m_labelVisible && !m_labelText.isEmpty()) {
        const int x = kLeftInset + kIconColumn + kTextGap;
        QFont f = font();
        f.setWeight(isChecked() ? QFont::DemiBold : QFont::Normal);
        p.setFont(f);
        p.setPen(isChecked() ? theme::kTextPrimary() : theme::kTextMuted());
        p.drawText(QRect(x, 0, width() - x - kLeftInset, height()),
                   Qt::AlignLeft | Qt::AlignVCenter, m_labelText);
    }

    // 4. Focus ring, so keyboard navigation remains visible.
    if (hasFocus()) {
        QStyleOptionFocusRect fr;
        fr.initFrom(this);
        fr.rect = rect().adjusted(2, 2, -2, -2);
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &fr, &p, this);
    }
}

} // namespace net
