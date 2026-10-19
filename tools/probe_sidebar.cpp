// Renders the sidebar navigation buttons exactly as the application configures
// them, so the icon problem can be seen and fixed rather than guessed at.
//
// The reported symptom: the icons appeared as small clipped fragments (a stubby
// arc, a short dash, a corner, a blob) rather than the intended shapes.
//
// Candidate causes this reproduces:
//   * the QPushButton stylesheet's padding shrinks the rect the icon is drawn in
//   * text and icon competing for the same row with text-align: left
//   * an iconSize smaller than the pixmaps provided

#include "navicons.h"
#include "theme.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

using namespace net;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);

    const icons::Kind kinds[6] = {
        icons::Kind::Dashboard, icons::Kind::Limiter,  icons::Kind::Scheduler,
        icons::Kind::AppLimiter, icons::Kind::Statistics, icons::Kind::Settings,
    };
    const char *labels[6] = {"Dashboard", "Limiter", "Scheduler",
                             "AppLimiter", "Statistics", "Settings"};

    auto *host = new QWidget;
    host->setObjectName(QStringLiteral("sidebar"));
    host->resize(210, 300);
    theme::applyApplicationTheme();
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);

    for (int i = 0; i < 6; ++i) {
        auto *b = new QPushButton(host);
        b->setObjectName(QStringLiteral("navItem"));
        b->setCheckable(true);
        b->setMinimumHeight(38);
        b->setIconSize(QSize(18, 18));
        b->setIcon(icons::icon(kinds[i], i == 0 ? theme::kTextPrimary() : theme::kTextMuted()));
        b->setText(QStringLiteral("  %1").arg(QString::fromLatin1(labels[i])));
        layout->addWidget(b);
    }
    layout->addStretch(1);
    host->show();

    // Give the stylesheet a chance to polish before grabbing.
    QApplication::processEvents();
    QApplication::processEvents();

    const QPixmap shot = host->grab();
    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("sidebar_probe.png");
    shot.save(out, "PNG");

    // Also print the geometry so the numbers are visible, not just the picture.
    FILE *f = fopen("sidebar_probe.txt", "w");
    for (QPushButton *b : host->findChildren<QPushButton *>()) {
        fprintf(f, "%-12s widget=%dx%d  contentsRect=%dx%d  iconSize=%dx%d  text='%s'\n",
                qPrintable(b->text().trimmed()),
                b->width(), b->height(),
                b->contentsRect().width(), b->contentsRect().height(),
                b->iconSize().width(), b->iconSize().height(),
                qPrintable(b->text()));
    }
    fclose(f);
    return 0;
}
