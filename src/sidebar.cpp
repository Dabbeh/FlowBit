#include "sidebar.h"

#include "brandmark.h"
#include "navicons.h"
#include "navbutton.h"
#include "theme.h"

#include <QButtonGroup>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace net {

namespace {

struct Item {
    Sidebar::Page page;
    icons::Kind icon;    // drawn with QPainter - see src/navicons.h
    const char *label;
    const char *tip;
};

// Order matches the reference mockup. AppLimiter sits after Scheduler because it is
// the same idea applied to a single application rather than the whole machine.
//
// The icons are DRAWN, not typed. The previous version used Unicode characters
// (U+25D4, U+2261, U+25F4, U+25A6, U+25A4, U+2699); Windows substitutes whichever
// font claims each codepoint, so the set rendered at mismatched weights and sizes
// and the gear came through a colour-emoji font. Choosing different characters
// cannot fix that - a text glyph is not an icon. See src/navicons.h.
const Item kItems[] = {
    {Sidebar::Dashboard, icons::Kind::Dashboard, "Dashboard", "Live speed, usage and a summary of the active limits"},
    {Sidebar::Scheduler, icons::Kind::Scheduler, "Scheduler", "Automatic bandwidth limits by time and day"},
    {Sidebar::AppLimiter, icons::Kind::AppLimiter, "App Limiter", "Bandwidth limits for individual applications"},
    {Sidebar::Statistics, icons::Kind::Statistics, "Statistics", "Data usage history and quota"},
    {Sidebar::Settings, icons::Kind::Settings, "Settings", "Appearance, diagnostics and application options"},
};

} // namespace

Sidebar::Sidebar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("sidebar"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(6);

    // -- navigation --------------------------------------------------------
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    for (const Item &item : kItems) {
        // NavButton paints its own icon. A plain QPushButton with setIcon() came
        // out with the icon clipped to a fragment under this stylesheet, because
        // Qt lays the icon out inside the padded content rect.
        auto *button = new NavButton(this);
        button->setIconKind(item.icon);
        button->setLabelText(QString::fromLatin1(item.label));
        button->setProperty("tip", QString::fromLatin1(item.tip));
        button->setToolTip(QString::fromLatin1(item.tip));

        m_group->addButton(button, int(item.page));
        m_buttons.append(button);
        outer->addWidget(button);

        connect(button, &QPushButton::clicked, this, [this, item] {
            setCurrentPage(item.page);
        });
    }

    outer->addStretch(1);

    // -- collapse toggle ---------------------------------------------------
    m_collapseButton = new QPushButton(this);
    m_collapseButton->setObjectName(QStringLiteral("navItem"));
    m_collapseButton->setCursor(Qt::PointingHandCursor);
    m_collapseButton->setAutoDefault(false);
    m_collapseButton->setMinimumHeight(30);
    m_collapseButton->setToolTip(QStringLiteral("Collapse or expand the sidebar"));
    outer->addWidget(m_collapseButton);

    connect(m_collapseButton, &QPushButton::clicked, this, [this] {
        setCollapsed(!m_collapsed);
    });

    m_buttons.first()->setChecked(true);
    applyState();
}

QPushButton *Sidebar::buttonFor(Page page) const
{
    const int i = int(page);
    return (i >= 0 && i < m_buttons.size()) ? m_buttons.at(i) : nullptr;
}

void Sidebar::setCurrentPage(Page page)
{
    if (page < 0 || page >= PageCount)
        return;

    m_current = page;
    if (QPushButton *b = buttonFor(page)) {
        if (!b->isChecked())
            b->setChecked(true);
    }
    emit pageSelected(int(page));
}

void Sidebar::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;
    applyState();
    emit collapsedChanged(m_collapsed);
}

void Sidebar::setNarrow(bool narrow)
{
    if (m_narrow == narrow)
        return;
    m_narrow = narrow;
    applyState();
}

void Sidebar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The rail is fixed-width; nothing to reflow, but the buttons need their
    // text re-elided to the new width when expanded.
    applyState();
}

void Sidebar::refreshTheme()
{
    // The brand block that used to live here was removed - the window's caption carries
    // the name and the mark appears in the taskbar - so there is no per-theme pixmap to
    // re-render. The buttons paint from the theme at paint time, and the style is
    // re-polished below for the sheet's own colours.
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void Sidebar::applyState()
{
    // Narrow windows force the collapsed rail regardless of the user's choice,
    // so the content area keeps a usable width.
    const bool collapsed = m_collapsed || m_narrow;

    setFixedWidth(collapsed ? kCollapsedWidth : kExpandedWidth);

    for (QPushButton *b : std::as_const(m_buttons)) {
        auto *nav = qobject_cast<NavButton *>(b);
        if (!nav)
            continue;

        // Collapsed: icon only, name in the tooltip. Expanded: icon plus label.
        // NavButton owns both, so nothing here can squeeze the icon.
        nav->setLabelVisible(!collapsed);
        nav->setToolTip(collapsed ? nav->accessibleName()
                                  : b->property("tip").toString());
    }

    if (m_collapseButton) {
        m_collapseButton->setText(collapsed ? QStringLiteral("\u00BB")
                                            : QStringLiteral("\u00AB   Collapse"));
        m_collapseButton->setToolTip(collapsed ? QStringLiteral("Expand the sidebar")
                                               : QStringLiteral("Collapse the sidebar"));
    }

    style()->unpolish(this);
    style()->polish(this);
    update();
}

} // namespace net
