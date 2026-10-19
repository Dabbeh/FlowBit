// Renders the real MainWindow in both themes at full size, then prints an
// ASCII luminance map so the layout and contrast can be judged without a display.
#include "../src/mainwindow.h"
#include "../src/settingsstore.h"
#include "../src/speedmonitor.h"
#include "../src/theme.h"
#include "../src/throttleengine.h"
#include "../src/usagetracker.h"
#include "../src/schedule.h"

#include <QApplication>
#include <QDir>

#include <cstdio>

using namespace net;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString scratch =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("shot"));
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    SettingsStore store(scratch);
    {
        Settings s = store.load();
        Rule r;
        r.enabled = true;
        r.name = QStringLiteral("Work Hours");
        r.from = QStringLiteral("09:00");
        r.to = QStringLiteral("17:30");
        r.days = 0x1F;
        r.down = 4u * 1024 * 1024;
        r.up = 512u * 1024;
        s.rules.append(r);
        QString err;
        store.save(s, &err);
    }

    ThrottleEngine engine;
    SpeedMonitor monitor;
    UsageTracker usage;
    usage.setDataDirectory(scratch);

    const char *modeArg = argc > 1 ? argv[1] : "light";
    const bool wantDark = QString::fromLatin1(modeArg) == QLatin1String("dark");
    theme::setMode(wantDark ? theme::Mode::Dark : theme::Mode::Light);

    MainWindow window(&store, &engine, &monitor, &usage);
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();
    QApplication::processEvents();

    const QImage img = window.grab().toImage();
    const QString out = QStringLiteral("shot-%1.png").arg(QString::fromLatin1(modeArg));
    img.save(out);
    std::printf("wrote %s (%dx%d)\n", qPrintable(out), img.width(), img.height());

    // ASCII map: each cell is the mean luminance of an 8x8 block.
    const int cell = 8;
    std::printf("luminance map (0=black 9=white, . = transparent)\n");
    for (int y = 0; y + cell <= img.height(); y += cell) {
        for (int x = 0; x + cell <= img.width(); x += cell) {
            double sum = 0;
            int n = 0;
            for (int dy = 0; dy < cell; ++dy)
                for (int dx = 0; dx < cell; ++dx) {
                    const QRgb c = img.pixel(x + dx, y + dy);
                    if (qAlpha(c) < 128)
                        continue;
                    sum += 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c);
                    ++n;
                }
            if (n == 0) {
                std::printf(".");
                continue;
            }
            const double L = sum / n;
            const int d = qBound(0, int(L / 25.6), 9);
            std::printf("%d", d);
        }
        std::printf("\n");
    }

    QDir(scratch).removeRecursively();
    return 0;
}
