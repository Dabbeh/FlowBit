#include "appactivitypanel.h"
#include "appmonitor.h"
#include "mainwindow.h"
#include "responsivepair.h"
#include "settingsstore.h"
#include "speedgraph.h"
#include "speedmonitor.h"
#include "theme.h"
#include "throttleengine.h"
#include "usagetracker.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

using namespace net;

namespace {
int failures = 0;
void check(bool passed, const char *message)
{
    std::printf("  %s: %s\n", passed ? "PASS" : "FAIL", message);
    if (!passed)
        ++failures;
}

void pump(int milliseconds)
{
    QElapsedTimer clock;
    clock.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    } while (clock.elapsed() < milliseconds);
}

struct Frame {
    int panelHeight;
    int graphY;
    int graphHeight;
    int tableY;
    int tableHeight;
    int scroll;
    int panelY;
    int limiterY;
    int schedulerY;
    int canvasHeight;
    bool tableVisible;
};

class PaintObserver : public QObject {
public:
    std::function<void()> observe;
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Paint && observe)
            observe();
        return QObject::eventFilter(object, event);
    }
};
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FlowbitDashboardAnimationTest"));
    // The offscreen plugin does not discover Windows fonts consistently. Load
    // the real UI fonts so text wrapping and card measurements remain realistic.
    for (const char *font : {"segoeui.ttf", "segoeuib.ttf", "consola.ttf", "consolab.ttf"})
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/") + font);
    theme::applyApplicationTheme();
    check(QFontMetrics(theme::uiFont(9)).inFont(QChar('A')),
          "the dashboard uses actual font glyphs for its layout measurements");
    QTemporaryDir fixtureDir(QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("scratch-XXXXXX")));
    if (!fixtureDir.isValid())
        return 2;
    const QString scratch = fixtureDir.path();
    SettingsStore store(scratch);
    Settings fixture;
    fixture.checkForUpdates = false;
    fixture.usage.cycleStart = QDate::currentDate().addDays(-4);
    if (!store.save(fixture))
        return 2;
    ThrottleEngine engine;
    SpeedMonitor speed;
    UsageTracker usage;
    usage.setDataDirectory(scratch);
    usage.load(fixture.usage);
    usage.resetCurrentCycle(fixture.usage);
    constexpr quint64 gib = 1024ull * 1024 * 1024;
    usage.addSample(8 * gib, 2 * gib);
    usage.flush();
    MainWindow window(&store, &engine, &speed, &usage);
    speed.stop();
    auto *monitor = window.findChild<AppMonitor *>();
    monitor->stop();
    monitor->forceRunningForTesting();
    for (auto *timer : window.findChildren<QTimer *>())
        timer->stop();

    auto *panel = window.findChild<AppActivityPanel *>();
    auto *graph = window.findChild<SpeedGraph *>();
    auto *table = panel->findChild<QTableWidget *>();
    QScrollArea *scroll = nullptr;
    for (auto *candidate : window.findChildren<QScrollArea *>())
        if (candidate->isAncestorOf(panel))
            scroll = candidate;
    QPushButton *toggle = nullptr;
    for (auto *button : window.findChildren<QPushButton *>())
        if (button->text().startsWith(QStringLiteral("Show More")))
            toggle = button;
    if (!scroll || !toggle || !graph || !panel || !table)
        return 2;
    auto *canvasLayout = scroll->widget()->layout();
    auto *limiter = canvasLayout->itemAt(canvasLayout->count() - 3)->widget();
    auto *scheduler = canvasLayout->itemAt(canvasLayout->count() - 2)->widget();
    const auto clickToggle = [&] { QTest::mouseClick(toggle, Qt::LeftButton); };
    auto *downloaded = window.findChild<QLabel *>(QStringLiteral("usageDownloaded"));
    auto *uploaded = window.findChild<QLabel *>(QStringLiteral("usageUploaded"));
    auto *projection = window.findChild<QLabel *>(QStringLiteral("usageProjection"));
    check(downloaded && uploaded && downloaded->text() == formatBytes(double(8 * gib))
          && uploaded->text() == formatBytes(double(2 * gib)),
          "the quota card restores the exact Downloaded and Uploaded cycle totals");
    check(projection && projection->text().contains(formatBytes(double(60 * gib))),
          "the quota card restores the projection for the full 30-day cycle");

    const auto sample = [&] {
        return Frame{panel->height(), graph->mapTo(scroll->viewport(), QPoint()).y(),
                     graph->height(), table->mapTo(panel, QPoint()).y(), table->height(),
                     scroll->verticalScrollBar()->value(),
                     panel->mapTo(scroll->viewport(), QPoint()).y(),
                     limiter->mapTo(scroll->viewport(), QPoint()).y(),
                     scheduler->mapTo(scroll->viewport(), QPoint()).y(), scroll->widget()->height(),
                     table->isVisible()};
    };
    // A transient bad layout can paint, then be corrected before processEvents
    // returns. Include those actual paint-time positions in the motion checks.
    std::vector<Frame> *paintingFrames = nullptr;
    PaintObserver paintObserver;
    paintObserver.observe = [&] { if (paintingFrames) paintingFrames->push_back(sample()); };
    panel->installEventFilter(&paintObserver);
    const auto record = [&](int milliseconds) {
        std::vector<Frame> frames{sample()};
        paintingFrames = &frames;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < milliseconds) {
            pump(5);
            frames.push_back(sample());
        }
        paintingFrames = nullptr;
        return frames;
    };
    const auto checkMotion = [&](const std::vector<Frame> &frames, int direction) {
        int backtrack = 0, largestStep = 0, panelMovement = 0;
        for (size_t i = 1; i < frames.size(); ++i) {
            for (int delta : {frames[i].panelHeight - frames[i - 1].panelHeight,
                              frames[i].limiterY - frames[i - 1].limiterY,
                              frames[i].schedulerY - frames[i - 1].schedulerY}) {
                backtrack = std::max(backtrack, -direction * delta);
                largestStep = std::max(largestStep, std::abs(delta));
            }
            panelMovement = std::max(panelMovement, std::abs(frames[i].panelY - frames.front().panelY));
            if (frames[i].panelY != frames[i - 1].panelY)
                std::printf("  panel top changed: h=%d y=%d -> h=%d y=%d scroll=%d\n",
                            frames[i - 1].panelHeight, frames[i - 1].panelY,
                            frames[i].panelHeight, frames[i].panelY, frames[i].scroll);
        }
        std::printf("  full motion: backtrack=%d largestStep=%d panelY travel=%d canvas=%d -> %d\n",
                    backtrack, largestStep, panelMovement, frames.front().canvasHeight, frames.back().canvasHeight);
        check(backtrack <= 1, "activity and lower cards never reverse direction during a transition");
        check(largestStep < 40, "activity and lower cards have no abrupt height or position jumps");
        check(panelMovement <= 1, "the activity panel's top stays anchored while toggling at the top");
    };

    // The first network interval can arrive while the empty-state reveal is
    // still running. It must not replace the content or snap the current height.
    window.resize(880, 650);
    window.show();
    pump(150);
    clickToggle();
    check(panel->isAnimating() && panel->height() <= 1,
          "the first reveal starts collapsed even when show delivers a pending resize");
    record(90);
    const int emptyHeight = panel->height();
    for (quint32 i = 0; i < 6; ++i)
        monitor->injectForTesting(7000 + i, QStringLiteral("C:/apps/app%1.exe").arg(i),
                                  2000000 - i * 1000, 20000);
    monitor->flushForTesting(1.0);
    panel->refresh();
    check(panel->height() == emptyHeight && !table->isVisible(),
          "new readings wait for the in-progress empty-state reveal to settle");
    const auto lateData = record(700);
    bool monotonic = true;
    for (size_t i = 1; i < lateData.size(); ++i)
        monotonic &= lateData[i].panelHeight >= lateData[i - 1].panelHeight;
    check(monotonic && table->isVisible() && table->rowCount() == 5,
          "the first readings grow into the populated state without snapping backwards");
    const auto appBytes = [&] {
        quint64 bytes = 0;
        for (const auto &entry : usage.appUsageOn(QDate::currentDate()))
            bytes += entry.total();
        return bytes;
    };
    QMetaObject::invokeMethod(monitor, "updated", Qt::DirectConnection);
    check(appBytes() == 12105000,
          "each completed application interval records its exact raw bytes once");
    const quint64 beforeSpeedTicks = appBytes();
    for (int i = 0; i < 2; ++i)
        QMetaObject::invokeMethod(&speed, "sampled", Qt::DirectConnection,
                                 Q_ARG(double, 1000000.0), Q_ARG(double, 2000000.0));
    check(appBytes() == beforeSpeedTicks,
          "speed ticks cannot count a deferred application snapshot a second time");
    check(usage.cycleTotalBytes() == 10 * gib,
          "quota usage uses adapter byte deltas rather than assuming rates cover one second");
    clickToggle();
    record(400);

    for (const QSize size : {QSize(880, 650), QSize(880, 900), QSize(1000, 650),
                             QSize(1140, 650), QSize(1160, 650), QSize(1180, 650),
                             QSize(1200, 760), QSize(1536, 816),
                             QSize(1920, 1040)}) {
        std::printf("\nDashboard %dx%d\n", size.width(), size.height());
        window.resize(size);
        window.show();
        pump(150);
        bool quotaFits = downloaded && uploaded && projection;
        if (quotaFits) {
            QWidget *card = downloaded->parentWidget();
            for (auto *label : {downloaded, uploaded, projection})
                quotaFits &= label->isVisible() && card->rect().contains(label->geometry());
            for (auto *button : card->findChildren<QPushButton *>())
                quotaFits &= button->isVisible() && card->rect().contains(button->geometry());
        }
        check(quotaFits, "quota readings, projection and reset controls fit inside their card");
        scroll->verticalScrollBar()->setValue(0);
        const Frame before = sample();
        clickToggle();
        auto opening = record(400);
        opening.insert(opening.begin(), before);
        checkMotion(opening, 1);
        const Frame opened = sample();
        int graphMovement = 0, graphResize = 0, internalMovement = 0;
        bool contentContinuous = true;
        for (const auto &frame : opening) {
            graphMovement = std::max(graphMovement, std::abs(frame.graphY - before.graphY));
            graphResize = std::max(graphResize, std::abs(frame.graphHeight - before.graphHeight));
            internalMovement = std::max(internalMovement,
                                       std::abs(frame.tableHeight - opened.tableHeight));
            if (frame.panelHeight > 0)
                contentContinuous &= frame.tableVisible && frame.tableY == opened.tableY;
        }
        std::printf("  graph movement=%d resize=%d; table height change=%d; opened panel=%d\n",
                    graphMovement, graphResize, internalMovement, opened.panelHeight);
        check(graphMovement <= 1 && graphResize <= 1,
              "opening keeps the actual live graph in place at its original size");
        check(internalMovement <= 1, "opening clips stable content instead of resizing the table");
        check(contentContinuous, "the same live table stays in place throughout opening");

        clickToggle();
        record(100);
        const int beforeReverse = panel->height();
        clickToggle();
        const int afterReverse = panel->height();
        std::printf("  reverse height %d -> %d\n", beforeReverse, afterReverse);
        check(std::abs(afterReverse - beforeReverse) <= 1,
              "reopening during collapse continues from the visible height");
        checkMotion(record(400), 1);
        const Frame beforeClosing = sample();
        clickToggle();
        auto closing = record(400);
        closing.insert(closing.begin(), beforeClosing);
        checkMotion(closing, -1);
        graphMovement = 0;
        graphResize = 0;
        contentContinuous = true;
        for (const auto &frame : closing) {
            graphMovement = std::max(graphMovement, std::abs(frame.graphY - before.graphY));
            graphResize = std::max(graphResize, std::abs(frame.graphHeight - before.graphHeight));
            if (frame.panelHeight > 0)
                contentContinuous &= frame.tableVisible && frame.tableY == opened.tableY;
        }
        std::printf("  collapse graph movement=%d resize=%d; scroll %d -> %d\n",
                    graphMovement, graphResize, closing.front().scroll, closing.back().scroll);
        check(graphMovement <= 1 && graphResize <= 1,
              "collapse keeps the actual live graph in place at its original size");
        check(contentContinuous, "the same live table stays in place throughout closing");

        clickToggle();
        record(90);
        const int beforeCloseReverse = panel->height();
        clickToggle();
        check(std::abs(panel->height() - beforeCloseReverse) <= 1,
              "collapsing during the reveal continues from the visible height");
        checkMotion(record(400), -1);

        clickToggle();
        record(400);
        if (size.width() == 1536)
            window.grab().save(QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("dashboard-open.png")));
        scroll->verticalScrollBar()->setValue(qMin(scroll->verticalScrollBar()->maximum(),
            panel->mapTo(scroll->widget(), QPoint()).y()));
        for (auto *button : panel->findChildren<QPushButton *>()) {
            if (button->text().startsWith(QStringLiteral("Show Less"))) {
                button->setFocus(Qt::MouseFocusReason);
                pump(20);
                const int scrollBefore = scroll->verticalScrollBar()->value();
                QTest::mouseClick(button, Qt::LeftButton);
                std::printf("  focused footer click: scroll %d -> %d\n", scrollBefore,
                            scroll->verticalScrollBar()->value());
                check(scrollBefore == scroll->verticalScrollBar()->value(),
                      "clicking the focused footer does not jump the viewport before animating");
                break;
            }
        }
        const auto scrolledClose = record(700);
        bool scrollMonotonic = true;
        int largestScrollStep = 0;
        int scrolledCardMovement = 0;
        for (size_t i = 1; i < scrolledClose.size(); ++i) {
            scrollMonotonic &= scrolledClose[i].scroll <= scrolledClose[i - 1].scroll;
            const int scrollDelta = scrolledClose[i].scroll - scrolledClose[i - 1].scroll;
            if (scrollDelta > 0 || std::abs(scrollDelta) >= 40)
                std::printf("  scroll jump: h=%d scroll=%d -> h=%d scroll=%d canvas=%d\n",
                            scrolledClose[i - 1].panelHeight, scrolledClose[i - 1].scroll,
                            scrolledClose[i].panelHeight, scrolledClose[i].scroll,
                            scrolledClose[i].canvasHeight);
            largestScrollStep = std::max(largestScrollStep,
                std::abs(scrolledClose[i].scroll - scrolledClose[i - 1].scroll));
            if (scrolledClose[i].panelHeight > 0)
                scrolledCardMovement = std::max(scrolledCardMovement,
                    std::abs(scrolledClose[i].panelY + scrolledClose[i].scroll
                             - scrolledClose.front().panelY - scrolledClose.front().scroll));
        }
        check(!panel->isVisible() && scrollMonotonic,
              "collapsing from inside a scrolled panel never pushes the viewport back down");
        check(largestScrollStep < 40, "the scrolled collapse finishes without a viewport snap");
        check(scrolledCardMovement <= 1,
              "the scrolled card does not shake within the canvas at paint time");
        check(QApplication::focusWidget() == toggle,
              "closing the focused footer returns keyboard focus to Show More");
    }
    std::printf("\n%d failures\n", failures);
    return failures ? 1 : 0;
}
