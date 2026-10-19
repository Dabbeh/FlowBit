// Rasterizes the Flowbit SVGs with QSvgRenderer and writes the PNG size set.
//
// This exists so the icon pipeline has the *vector* as its single source of
// truth: the .ico and every PNG are produced from the SVG paths rather than
// from a pre-existing bitmap, which is what keeps 16px and 512px consistent.
//
// Usage: svg_render <in.svg> <out-prefix> [sizes...]
//        e.g. svg_render flowbit-dark.svg dark 16 24 32 48 64 128 256 512

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    if (argc < 3) {
        std::fprintf(stderr, "usage: svg_render <in.svg> <out-prefix> [sizes...]\n");
        return 2;
    }

    const QString input = QString::fromLocal8Bit(argv[1]);
    const QString prefix = QString::fromLocal8Bit(argv[2]);

    QSvgRenderer renderer(input);
    if (!renderer.isValid()) {
        std::fprintf(stderr, "invalid or unreadable SVG: %s\n", qPrintable(input));
        return 1;
    }

    QList<int> sizes;
    for (int i = 3; i < argc; ++i)
        sizes << QString::fromLocal8Bit(argv[i]).toInt();
    if (sizes.isEmpty())
        sizes = {16, 24, 32, 48, 64, 128, 256, 512};

    for (int s : sizes) {
        QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);

        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&p);
        p.end();

        const QString out = QStringLiteral("%1-%2.png").arg(prefix).arg(s);
        if (!img.save(out)) {
            std::fprintf(stderr, "failed to write %s\n", qPrintable(out));
            return 1;
        }
        std::printf("wrote %s (%dx%d)\n", qPrintable(out), s, s);
    }

    return 0;
}
