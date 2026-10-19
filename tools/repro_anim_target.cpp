// Proves the Show Less crash: an animation whose TARGET is deleted from the
// animation's own finished handler, while the animation itself is also marked for
// self-deletion.
//
// This is the pattern that existed in two places:
//
//   mainwindow.cpp (page cross-fade)
//       anim = new QPropertyAnimation(fx, "opacity", incoming);
//       connect(anim, finished, incoming, [incoming] {
//           incoming->setGraphicsEffect(nullptr);   // DELETES fx, the target
//       });
//       anim->start(QAbstractAnimation::DeleteWhenStopped);
//
//   mainwindow.cpp (dashboard scroll glide)
//       anim->start(QAbstractAnimation::DeleteWhenStopped);
//       ...and separately anim->deleteLater();       // deleted twice
//
// In both cases a Qt object is destroyed while an animation still references it,
// which shows up as 0xc0000005 inside Qt6Core.dll - the fault the Windows event
// log recorded for Flowbit.exe on Show Less, at a constant offset.
//
// Usage: repro_anim_target.exe [buggy|fixed]

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstring>

namespace {

QWidget *makePage(QWidget *parent)
{
    auto *page = new QWidget(parent);
    page->setMinimumHeight(120);
    return page;
}

// The page cross-fade, as it was and as it is now.
void switchPage(QWidget *incoming, bool fixed)
{
    auto *fx = new QGraphicsOpacityEffect(incoming);
    fx->setOpacity(0.0);
    incoming->setGraphicsEffect(fx);

    auto *anim = new QPropertyAnimation(fx, "opacity", incoming);
    anim->setDuration(60);          // short, so many cycles fit in the test
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);

    if (fixed) {
        QObject::connect(anim, &QPropertyAnimation::finished, incoming, [incoming, anim] {
            incoming->setGraphicsEffect(nullptr);
            anim->deleteLater();
        });
        anim->start();
    } else {
        QObject::connect(anim, &QPropertyAnimation::finished, incoming, [incoming] {
            incoming->setGraphicsEffect(nullptr);
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const bool fixed = (argc > 1) && (std::strcmp(argv[1], "fixed") == 0);
    std::printf("mode: %s\n", fixed ? "fixed" : "buggy (target deleted from its own finished handler)");

    // A scroll area with a bar, like the dashboard, plus a page to cross-fade.
    auto *win = new QWidget;
    win->resize(500, 400);
    auto *layout = new QVBoxLayout(win);

    auto *scroll = new QScrollArea(win);
    auto *inner = new QWidget;
    inner->setMinimumHeight(3000);
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);
    layout->addWidget(scroll);

    auto *page = makePage(win);
    layout->addWidget(page);

    win->show();

    // Switch pages repeatedly, and glide the scrollbar, interrupting both - which
    // is what clicking around the sidebar and toggling the panel produces.
    int step = 0;
    for (int i = 0; i < 60; ++i) {
        QTimer::singleShot(150 + i * 50, win, [page, scroll, &step, fixed] {
            if (step % 2 == 0) {
                switchPage(page, fixed);
            } else {
                auto *bar = scroll->verticalScrollBar();
                auto *anim = new QPropertyAnimation(bar, "value", scroll);
                anim->setDuration(80);
                anim->setStartValue(bar->value());
                anim->setEndValue((step * 137) % 2000);
                if (fixed) {
                    QObject::connect(anim, &QPropertyAnimation::finished, scroll, [anim] {
                        anim->deleteLater();
                    });
                    anim->start();
                } else {
                    // both, as the original did: self-delete AND queued delete
                    anim->start(QAbstractAnimation::DeleteWhenStopped);
                }
            }
            ++step;
        });
    }

    QTimer::singleShot(150 + 60 * 50 + 500, win, [win] {
        std::printf("SURVIVED all steps\n");
        win->close();
    });

    return app.exec();
}
