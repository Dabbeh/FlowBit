#include "appmonitor.h"
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>
using namespace net;
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    AppMonitor m;
    out << "session name: " << m.sessionName() << "\n";
    out << "leftover exists: " << (m.leftoverSessionExists() ? "yes" : "no") << "\n";
    out << "starting...\n";
    m.start();
    out << "state: " << int(m.state()) << "\n";
    out << "running: " << (m.isRunning() ? "yes" : "no") << "\n";
    out << "error: " << (m.errorString().isEmpty() ? QStringLiteral("(none)") : m.errorString()) << "\n";
    out << "session name now: " << m.sessionName() << "\n";
    out << "events seen: " << m.eventsSeenTotal() << "\n";
    // Drive ticks and report whether REAL per-app rows arrive.
    //
    // On this machine ETW session creation succeeds but the consumer receives zero
    // events (build 26300 behaviour), so the EStats source must take over after the
    // silence threshold. This prints the proof either way.
    int tick = 0;
    QTimer *t2 = new QTimer(&app);
    t2->setInterval(400);
    QObject::connect(t2, &QTimer::timeout, &app, [&]{
        m.tick();
        ++tick;
        const auto rows = m.snapshot();
        out << "tick " << tick
            << "  events=" << m.eventsSeenTotal()
            << "  tcpFallback=" << (m.usingTcpFallback() ? "yes" : "no")
            << "  apps=" << rows.size() << "\n";
        if (tick >= 8) {
            double total = 0;
            for (const auto &r : rows)
                total += r.downBytesPerSec + r.upBytesPerSec;
            out << "total observed rate across apps: " << qint64(total) << " B/s\n";
            if (!rows.isEmpty()) {
                const auto &top = rows.first();
                out << "top app: " << top.displayName
                    << "  down=" << qint64(top.downBytesPerSec) << " B/s"
                    << "  up=" << qint64(top.upBytesPerSec) << " B/s\n";
            }
            out << (rows.isEmpty() ? "RESULT: NO DATA\n" : "RESULT: REAL DATA PRESENT\n");
            t2->stop();
            m.stop();
            out << "stopped\n";
            app.quit();
        }
    });
    t2->start();
    return app.exec();
}
