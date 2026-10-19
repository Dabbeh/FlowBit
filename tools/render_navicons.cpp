// Renders the six sidebar icons to a PNG sheet for visual inspection.
//
// The previous icon set looked wrong on screen and could not be judged from code,
// so this produces an actual image: every icon at the sizes actually used (18px on
// a 38px row) plus a large version to show the shapes the user will see at a
// glance. Run it after any change to navicons.cpp.

#include "navicons.h"
#include "theme.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

using namespace net;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Dark theme, since that is the default and what the screenshot showed.
    theme::setMode(theme::Mode::Dark);

    QImage sheet(560, 260, QImage::Format_ARGB32);
    sheet.fill(theme::kSidebar());

    QPainter p(&sheet);
    p.setRenderHint(QPainter::Antialiasing, true);

    const icons::Kind kinds[6] = {
        icons::Kind::Dashboard, icons::Kind::Limiter,  icons::Kind::Scheduler,
        icons::Kind::AppLimiter, icons::Kind::Statistics, icons::Kind::Settings,
    };
    const char *names[6] = {"Dashboard", "Limiter", "Scheduler",
                            "AppLimiter", "Statistics", "Settings"};

    // Row 1: at the real on-screen size (18px) over a 38px row, in both the
    // selected (bright) and normal (muted) tints.
    p.setPen(theme::kTextMuted());
    p.setFont(theme::uiFont(9, false));
    p.drawText(12, 20, QStringLiteral("actual size (18 px), selected vs normal:"));

    for (int i = 0; i < 6; ++i) {
        const int x = 20 + i * 88;
        const QRectF cell(x, 34, 38, 38);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::kPanelSunken());
        p.drawRoundedRect(cell, 8, 8);

        icons::paint(p, kinds[i], QRectF(x + 10, 44, 18, 18), theme::kTextPrimary());
        icons::paint(p, kinds[i], QRectF(x + 46, 44, 18, 18), theme::kTextMuted());

        p.setPen(theme::kTextMuted());
        p.setFont(theme::uiFont(8, false));
        p.drawText(QRectF(x, 74, 80, 16), Qt::AlignCenter, QString::fromLatin1(names[i]));
    }

    // Row 2: large, so the shapes themselves can be checked.
    p.setPen(theme::kTextMuted());
    p.setFont(theme::uiFont(9, false));
    p.drawText(12, 116, QStringLiteral("enlarged (48 px), to inspect the shapes:"));

    for (int i = 0; i < 6; ++i) {
        const int x = 20 + i * 88;
        icons::paint(p, kinds[i], QRectF(x, 132, 48, 48), theme::kTextPrimary());
        p.setPen(theme::kBorder());
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(x, 132, 48, 48));
    }

    p.setPen(theme::kTextMuted());
    p.setFont(theme::uiFont(8, false));
    p.drawText(12, 210,
               QStringLiteral("All six share one stroke width and one 24x24 grid, drawn with "
                              "QPainter - no font dependency, so no emoji fallback and no "
                              "weight mismatch."));

    p.end();

    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("navicons.png");
    sheet.save(out, "PNG");
    return 0;
}
