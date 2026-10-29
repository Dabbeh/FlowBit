// Verifies the ETW consumer teardown order, which is a use-after-close if it is wrong.
//
// ProcessTrace() blocks on the consumer thread and holds the trace handle while it runs.
// CloseTrace() frees that handle. So the only correct order is:
//
//   stop the session  ->  join the consumer  ->  close the handle
//
// The code used to close the handle first, then join, then delete the QThread object even if the
// join timed out. Both halves are asserted here, in order, from the source - the sequence cannot be
// observed at runtime without racing a real crash.

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
    std::printf("\n== ETW teardown order ==\n");

    const QString src = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/appmonitor.cpp"));
    check(!src.isEmpty(), "the source is readable");

    const int fn = src.indexOf(QStringLiteral("void AppMonitor::Impl::stopSession()"));
    const int end = src.indexOf(QStringLiteral("bool AppMonitor::Impl::startSession"), fn);
    check(fn > 0 && end > fn, "stopSession is located");
    const QString body = src.mid(fn, end - fn);

    const int stopAt = body.indexOf(QStringLiteral("StopTraceW(session, sessionNameInUse()"));
    const int waitAt = body.indexOf(QStringLiteral("thread->wait("));
    const int closeAt = body.indexOf(QStringLiteral("CloseTrace(traceHandle)"));
    std::printf("  positions: stop=%d wait=%d close=%d\n", stopAt, waitAt, closeAt);

    check(stopAt > 0, "the session is stopped");
    check(waitAt > stopAt, "the consumer thread is joined after the session stops");
    check(closeAt > waitAt, "and the trace handle is closed only after the join");

    check(!body.contains(QStringLiteral("wait(3000)")), "the old 3s join is gone");
    check(body.contains(QStringLiteral("if (thread->wait(")),
          "the join result is checked, not ignored");
    check(body.contains(QStringLiteral("did not stop within")),
          "a join timeout is reported instead of deleting a running thread");
    {
        // deleting a running QThread is the crash; the guarded branch must be the only delete.
        const int delAt = body.indexOf(QStringLiteral("delete thread;"));
        const int ifAt = body.indexOf(QStringLiteral("if (thread->wait("));
        check(delAt > ifAt && delAt < closeAt,
              "the thread object is deleted only inside the successful-join branch");
    }

    std::printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
                g_failures == 0 ? "test_etw_teardown passed." : "FAILED.");
    return g_failures == 0 ? 0 : 1;
}
