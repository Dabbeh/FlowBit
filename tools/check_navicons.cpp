// Checks what is actually inside the icon pixmaps.
//
// The sidebar probe reproduced the reported symptom exactly: the icons render as
// small clipped fragments. The button geometry is correct (18x18 icon in a 190x38
// button), so the fault must be in the pixmap contents or in how paint() maps the
// 24x24 grid onto the requested rect.
//
// This draws each icon into a large transparent canvas and reports the bounding
// box of the pixels that are actually painted. If the bounding box is much smaller
// than the canvas, the icon is being drawn at the wrong scale - which is what a
// fragment looks like on screen.

#include "navicons.h"
#include "theme.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cstdio>

using namespace net;

namespace {

// Bounding box of non-transparent pixels.
QRect contentBounds(const QImage &img)
{
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) > 8) {
                x0 = qMin(x0, x);
                y0 = qMin(y0, y);
                x1 = qMax(x1, x);
                y1 = qMax(y1, y);
            }
        }
    }
    if (x1 < 0)
        return QRect();
    return QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    theme::setMode(theme::Mode::Dark);

    const icons::Kind kinds[6] = {
        icons::Kind::Dashboard, icons::Kind::Limiter,  icons::Kind::Scheduler,
        icons::Kind::AppLimiter, icons::Kind::Statistics, icons::Kind::Settings,
    };
    const char *names[6] = {"Dashboard", "Limiter", "Scheduler",
                            "AppLimiter", "Statistics", "Settings"};

    FILE *f = fopen("navicons_bounds.txt", "w");
    fprintf(f, "%-12s %-10s %-14s %s\n", "icon", "pixmap", "painted bbox", "verdict");
    fprintf(f, "%s\n", QByteArray(60, '-').constData());

    // Draw a 6x1 strip at 48px for visual confirmation.
    QImage strip(6 * 64, 64, QImage::Format_ARGB32);
    strip.fill(theme::kSidebar());
    {
        QPainter p(&strip);
        for (int i = 0; i < 6; ++i)
            icons::paint(p, kinds[i], QRectF(i * 64 + 8, 8, 48, 48), theme::kTextPrimary());
    }
    strip.save("navicons_strip.png", "PNG");

    int problems = 0;
    for (int i = 0; i < 6; ++i) {
        for (int size : {18, 24, 48}) {
            const QPixmap pm = icons::pixmap(kinds[i], size, Qt::white, 1.0);
            const QImage img = pm.toImage();
            const QRect bb = contentBounds(img);

            // A correct icon nearly fills its box: the 24x24 grid draws content
            // from about 4 to 20, so the painted box should be a good fraction of
            // the pixmap. Anything tiny means the scale is wrong.
            const double fill = bb.isEmpty()
                                    ? 0.0
                                    : double(bb.width() * bb.height())
                                          / double(img.width() * img.height());
            const bool bad = fill < 0.25;
            if (bad)
                ++problems;

            fprintf(f, "%-12s %-10s %-14s %s\n", names[i],
                    qPrintable(QStringLiteral("%1x%1").arg(size)),
                    qPrintable(bb.isEmpty()
                                   ? QStringLiteral("(empty)")
                                   : QStringLiteral("%1x%2 at %3,%4")
                                         .arg(bb.width()).arg(bb.height())
                                         .arg(bb.x()).arg(bb.y())),
                    bad ? "TOO SMALL - drawn at the wrong scale" : "ok");
        }
    }
    fclose(f);

    printf("problems: %d\n", problems);
    return problems == 0 ? 0 : 1;
}
