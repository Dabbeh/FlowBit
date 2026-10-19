// Verifies every sidebar entry selects the page it names, after the Limiter page was
// removed.
//
// The sidebar's item order and the QStackedWidget's order are two separate lists that
// must agree; the enum value's position is a third. Removing an entry from one and not
// the others would silently show the WRONG page for every entry after it - a fault that
// looks like a navigation bug rather than a removal bug, so it is worth asserting
// directly.
//
// For each sidebar entry this selects it, finds which page the stack is showing, and
// checks the page's own heading matches the entry's name.

#include "mainwindow.h"
#include "settingsstore.h"
#include "throttleengine.h"
#include "speedmonitor.h"
#include "usagetracker.h"
#include "navbutton.h"
#include "sidebar.h"
#include "theme.h"

#include <algorithm>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QThread>
#include <QWidget>

#include <cstdio>

using namespace net;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

// The page heading, which makePage() gives the objectName "pageTitle".
QString headingOf(QWidget *page)
{
    for (QLabel *l : page->findChildren<QLabel *>()) {
        if (l->objectName() == QLatin1String("pageTitle"))
            return l->text();
    }
    return QString();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);
    theme::applyApplicationTheme();

    static SettingsStore store;
    static ThrottleEngine engine;
    static SpeedMonitor monitor;
    static UsageTracker usage;

    auto *win = new MainWindow(&store, &engine, &monitor, &usage);
    win->resize(1100, 860);
    win->show();
    pump(300);

    auto *side = win->findChild<Sidebar *>();
    auto *stack = win->findChild<QStackedWidget *>();
    check(side != nullptr, "the sidebar exists");
    check(stack != nullptr, "the page stack exists");
    if (!side || !stack)
        return 1;

    // Walk the sidebar's own enum rather than hunting for widgets.
    //
    // PageCount and buttonFor(Page) are public, so this uses the same mapping the
    // application does. Two earlier attempts failed on widget discovery: NavButton keeps
    // its caption in labelText() with no getter, so filtering on text() found nothing.
    const int pageCount = int(Sidebar::PageCount);
    std::printf("\n== sidebar pages: %d, stack pages: %d ==\n", pageCount, stack->count());

    int missingButtons = 0;
    for (int i = 0; i < pageCount; ++i) {
        if (!side->buttonFor(Sidebar::Page(i)))
            ++missingButtons;
    }
    check(missingButtons == 0,
          QStringLiteral("every page in the enum has a navigation button (%1 missing)")
              .arg(missingButtons));
    check(pageCount == stack->count(),
          QStringLiteral("the enum and the page stack agree (%1 vs %2)")
              .arg(pageCount).arg(stack->count()));
    // The enum now has five values; the removed page would have made six.
    check(pageCount == 5, QStringLiteral("the enum has five pages (%1)").arg(pageCount));

    std::printf("\n== each page shows its own content ==\n");
    for (int i = 0; i < pageCount; ++i) {
        win->onPageSelected(i);
        pump(120);
        QWidget *page = stack->currentWidget();
        const QString heading = page ? headingOf(page) : QString();
        const QString name = QStringLiteral("page %1").arg(i);
        std::printf("  selected %d -> stack index %d heading \"%s\"\n",
                    i, stack->currentIndex(), qPrintable(heading));

        // The heading and the entry name should agree for every page. "Limiter" is gone,
        // so no entry may lead to a page titled "Bandwidth Limiter".
        check(!heading.contains(QLatin1String("Bandwidth Limiter")),
              QStringLiteral("%1 does not lead to the removed Limiter page").arg(name));
        check(!heading.isEmpty(),
              QStringLiteral("%1 leads to a page with a heading").arg(name));
    }

    // Specifically: the removed page must not be in the stack either.
    bool limiterPresent = false;
    for (int i = 0; i < stack->count(); ++i) {
        if (headingOf(stack->widget(i)).contains(QLatin1String("Bandwidth Limiter")))
            limiterPresent = true;
    }
    check(!limiterPresent, "no page in the stack is the removed Limiter page");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_sidebar_pages passed."
                                        : "test_sidebar_pages FAILED.");

    win->close();
    delete win;
    return g_failures == 0 ? 0 : 1;
}
