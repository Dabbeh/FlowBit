// Pins the fixes from docs/audit-findings.md, so none of them can quietly regress.
//
// Each check names the defect it guards. They are source-level assertions because the states
// involved - a full injection queue, schtasks hanging, a start in flight during collapse - cannot
// be produced on demand in a test process.

#include <QCoreApplication>
#include <QFile>
#include <cstdio>

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok) ++g_failures;
}
QString read(const QString &p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("\n== audit fixes ==\n");

    const QString eng = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/applimitengine.cpp"));
    const QString win = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/mainwindow.cpp"));
    const QString panel = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/appactivitypanel.cpp"));
    const QString store = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/settingsstore.cpp"));
    const QString throttle = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/throttleengine.cpp"));
    check(!eng.isEmpty() && !win.isEmpty() && !panel.isEmpty() && !store.isEmpty() && !throttle.isEmpty(),
          "the sources are readable");

    // F1 - re-injection results are checked
    check(eng.contains(QStringLiteral("injectOrCount")), "F1: injection goes through a checked helper");
    check(eng.contains(QStringLiteral("m_injectFailures")), "F1: losses are counted");
    check(eng.contains(QStringLiteral("ERROR_INSUFFICIENT_BUFFER")), "F1: retries only on transient codes");
    {
        // No WinDivertSend may be called without its result being used.
        int bare = 0;
        const QStringList lines = eng.split(QLatin1Char('\n'));
        for (const QString &l : lines) {
            const QString t = l.trimmed();
            if (t.startsWith(QStringLiteral("WinDivertSend("))) ++bare;
        }
        check(bare == 0, "F1: no bare WinDivertSend call remains at statement position");
    }

    // F2 - startup registration off the GUI thread
    check(win.contains(QStringLiteral("QThread::create([this, want]")),
          "F2: schtasks work runs on a worker thread");
    check(win.contains(QStringLiteral("onStartupSettingApplied")),
          "F2: the UI is updated through a completion handler");
    check(win.contains(QStringLiteral("m_startupPending")),
          "F2: a toggle during a running change is replayed, not dropped");

    // F3 - monitor teardown off the GUI thread
    check(!panel.contains(QStringLiteral("m_monitor->stop()")),
          "F3: the panel never calls the blocking stop()");
    check(panel.contains(QStringLiteral("m_monitor->stopAsync()")),
          "F3: it uses stopAsync() instead");

    // F4 - permissions are checked
    check(store.contains(QStringLiteral("if (!QFile::setPermissions")),
          "F4: a failed permission change is handled, not ignored");

    // F5 - no dead state in the repair
    check(!throttle.contains(QStringLiteral("RepairOutcome outcome")),
          "F5: the unused 'outcome' variable is gone; the read-back decides");

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_audit_fixes passed." : "test_audit_fixes FAILED.");
    return g_failures == 0 ? 0 : 1;
}
