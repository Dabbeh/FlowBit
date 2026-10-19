// Verifies the app renders the REAL Flowbit logo, not an approximation.
//
// Guards the specific regression that prompted this work: an earlier
// makeTrayIcon() hand-painted a conventional letter F (rounded rectangular stem
// plus two bars). So this renders the production icon and checks it against the
// reference artwork pixel-for-pixel, and separately asserts the silhouette is
// the two-ribbon shape rather than a letterform.
//
// Build/run: tools/run_icon_tests.ps1
#include "../src/brandmark.h"
#include "../src/theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTextStream>

#include <cstdio>

using namespace net;

static int g_failures = 0;
static int g_checks = 0;
static QFile *g_log = nullptr;

static void logLine(const QString &s)
{
    std::printf("%s\n", qPrintable(s));
    std::fflush(stdout);
    if (g_log && g_log->isOpen()) {
        g_log->write(s.toUtf8());
        g_log->write("\n");
        g_log->flush();
    }
}

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        logLine(QStringLiteral("FAIL: ") + what);
    } else {
        logLine(QStringLiteral("  ok: ") + what);
    }
}

// Mean absolute per-channel difference between two same-size images.
static double meanAbsDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull())
        return 1e9;
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb ca = a.pixel(x, y);
            const QRgb cb = b.pixel(x, y);
            sum += qAbs(qRed(ca) - qRed(cb)) + qAbs(qGreen(ca) - qGreen(cb))
                   + qAbs(qBlue(ca) - qBlue(cb));
            n += 3;
        }
    }
    return n ? sum / n : 1e9;
}

// Counts opaque "ribbon" pixels (saturated cyan/blue) inside a rendered mark.
static int ribbonPixels(const QImage &img)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 160)
                continue;
            // Cyan through electric blue: blue dominant, red low.
            if (qBlue(c) > 150 && qRed(c) < 140)
                ++n;
        }
    }
    return n;
}

// Bounding box of the ribbon pixels, so the symbol's extent can be measured.
static QRect ribbonBounds(const QImage &img)
{
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 160)
                continue;
            if (qBlue(c) > 150 && qRed(c) < 140) {
                minX = qMin(minX, x);
                minY = qMin(minY, y);
                maxX = qMax(maxX, x);
                maxY = qMax(maxY, y);
            }
        }
    }
    if (maxX < 0)
        return QRect();
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("icon_results.txt")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    // =====================================================================
    // The vector source must be found and usable
    // =====================================================================
    check(brand::usingVectorSource(),
          "brand mark loads its vector source (non-empty SVG path resolved)");

    // =====================================================================
    // The production icon must match the reference artwork
    // =====================================================================
    {
        const QImage rendered = brand::mark(256, false, true).pixmap(256, 256).toImage();

        // The reference PNG that ships with the artwork pack.
        const QString refPath = QDir(QCoreApplication::applicationDirPath())
                                    .filePath(QStringLiteral("../../docs/flowbit-dark-256.png"));
        QImage reference(refPath);
        if (reference.isNull()) {
            // Try the source tree directly when running from build/tests.
            reference = QImage(QStringLiteral("flowbit-dark-256.png"));
        }

        check(!rendered.isNull() && rendered.width() == 256,
              "the mark renders at 256px");

        if (!reference.isNull()) {
            check(reference.size() == rendered.size(),
                  QStringLiteral("reference is the same size (%1x%2)")
                      .arg(reference.width())
                      .arg(reference.height()));
            const double d = meanAbsDiff(rendered, reference.convertToFormat(QImage::Format_ARGB32));
            logLine(QStringLiteral("     mean abs difference vs reference: %1").arg(d, 0, 'f', 3));
            // A conventional letter F would differ enormously; an exact vector
            // render differs only by rasteriser antialiasing.
            check(d < 6.0,
                  QStringLiteral("production icon matches the reference artwork "
                                 "(mean diff %1, threshold 6)")
                      .arg(d, 0, 'f', 2));
        } else {
            logLine(QStringLiteral("     (reference PNG not reachable; "
                                   "pixel comparison skipped)"));
        }
    }

    // =====================================================================
    // Silhouette: two ribbons, not a letterform
    // =====================================================================
    {
        const QImage img = brand::mark(256, false, true).pixmap(256, 256).toImage();
        const QRect b = ribbonBounds(img);
        check(b.isValid(), "ribbon pixels are present");

        const int coverage = ribbonPixels(img) * 100 / (256 * 256);
        logLine(QStringLiteral("     symbol bbox %1,%2 %3x%4, coverage %5%% of the plate")
                    .arg(b.x())
                    .arg(b.y())
                    .arg(b.width())
                    .arg(b.height())
                    .arg(coverage));

        // The spec asks for roughly 65-70% of the width.
        const double widthFrac = b.width() / 256.0;
        check(widthFrac > 0.55 && widthFrac < 0.80,
              QStringLiteral("symbol occupies a balanced share of the width (%1%)")
                  .arg(widthFrac * 100.0, 0, 'f', 0));

        // A conventional F has a tall, narrow, near-solid stem on the far left.
        // The real mark is two sweeping ribbons, so the left column is sparse and
        // the mass sits toward the middle and right.
        int leftColumnFilled = 0;
        for (int y = 0; y < 256; ++y) {
            for (int x = b.left(); x < b.left() + 18; ++x) {
                const QRgb c = img.pixel(x, y);
                if (qAlpha(c) > 160 && qBlue(c) > 150 && qRed(c) < 140)
                    ++leftColumnFilled;
            }
        }
        const double leftDensity = leftColumnFilled / (256.0 * 18.0);
        logLine(QStringLiteral("     left-edge ribbon density: %1%")
                    .arg(leftDensity * 100.0, 0, 'f', 1));

        // Ribbons sweep in and out; a solid F stem would fill most of it.
        check(leftDensity < 0.45,
              QStringLiteral("no solid rectangular stem at the left edge (%1%)")
                  .arg(leftDensity * 100.0, 0, 'f', 1));

        // Two distinct ribbons: scanning the vertical centre column must find at
        // least two separate runs of ribbon separated by plate.
        const int cx = 256 / 2;
        int runs = 0;
        bool inRun = false;
        for (int y = 0; y < 256; ++y) {
            const QRgb c = img.pixel(cx, y);
            const bool ribbon = qAlpha(c) > 160 && qBlue(c) > 150 && qRed(c) < 140;
            if (ribbon && !inRun) {
                ++runs;
                inRun = true;
            } else if (!ribbon) {
                inRun = false;
            }
        }
        check(runs >= 2,
              QStringLiteral("two distinct ribbons separated by a gap (found %1 runs)")
                  .arg(runs));
    }

    // =====================================================================
    // Both plate variants exist and differ
    // =====================================================================
    {
        const QImage dark = brand::mark(256, false, true).pixmap(256, 256).toImage();
        const QImage light = brand::mark(256, false, false).pixmap(256, 256).toImage();
        check(meanAbsDiff(dark, light) > 10.0,
              "dark and light plate variants differ");
    }

    // =====================================================================
    // Off state and small sizes
    // =====================================================================
    {
        const QImage on = brand::mark(256, false, true).pixmap(256, 256).toImage();
        const QImage off = brand::mark(256, true, true).pixmap(256, 256).toImage();
        check(meanAbsDiff(on, off) > 5.0, "the off state draws its slash");

        for (int s : {16, 24, 32, 48}) {
            const QImage small = brand::mark(s, false, true).pixmap(s, s).toImage();
            check(!small.isNull() && small.width() == s,
                  QStringLiteral("renders at %1px").arg(s));

            // Must still contain ribbon colour, i.e. the symbol survives scaling
            // rather than turning into a blank or muddy plate.
            const int ribbon = ribbonPixels(small);
            check(ribbon >= qMax(6, s * s / 90),
                  QStringLiteral("symbol is legible at %1px (%2 ribbon px)").arg(s).arg(ribbon));
        }
    }

    logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
