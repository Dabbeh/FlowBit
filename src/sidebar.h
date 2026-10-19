#pragma once

#include <QWidget>

class QButtonGroup;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace net {

// The permanent left navigation rail.
//
// Owns no application state beyond which page is selected: it reports the
// selection and the window decides what to show. That keeps the sidebar usable
// from tests and means a theme switch cannot disturb the selected page.
//
// Collapses to an icon-only rail when the window is too narrow to spare the
// expanded width, and can also be collapsed explicitly by the user.
class Sidebar : public QWidget
{
    Q_OBJECT

public:
    enum Page {
        Dashboard = 0,
        Scheduler,
        AppLimiter,
        Statistics,
        Settings,
        PageCount,
    };

    explicit Sidebar(QWidget *parent = nullptr);

    Page currentPage() const { return m_current; }
    void setCurrentPage(Page page);

    // Icon-only mode. Applied automatically when narrow, or toggled by the user.
    void setCollapsed(bool collapsed);
    bool isCollapsed() const { return m_collapsed; }

    // Set by the owner when the window is too narrow for the expanded rail. This
    // is separate from the user's explicit collapse so a resize does not
    // permanently override their choice.
    void setNarrow(bool narrow);
    bool isNarrow() const { return m_narrow; }

    QPushButton *buttonFor(Page page) const;

    // Re-renders the brand mark for the current theme. The mark's plate colour is
    // baked into a pixmap at construction, so a live theme switch would otherwise
    // leave a dark plate on the light rail.
    void refreshTheme();

signals:
    void pageSelected(int page);
    void collapsedChanged(bool collapsed);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyState();

    QPushButton *m_collapseButton = nullptr;
    QButtonGroup *m_group = nullptr;
    QVector<QPushButton *> m_buttons;

    Page m_current = Dashboard;
    bool m_collapsed = false;
    bool m_narrow = false;

    // Expanded rail width, per the reference (173px of rail plus window inset).
    static constexpr int kExpandedWidth = 196;
    static constexpr int kCollapsedWidth = 60;
};

} // namespace net
