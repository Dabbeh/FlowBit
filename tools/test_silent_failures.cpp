// Verifies that a failed save is reported rather than looking like success.
//
// The defect class: a settings write that fails (full disk, permissions, a read-only profile) left
// the UI showing the new value while settings.json kept the old one, so the change "applied" and
// then silently reverted at the next launch. Several call sites discarded both the bool and the
// error string.

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
    std::printf("\n== failures are reported, not swallowed ==\n");

    const QString win = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/mainwindow.cpp"));
    const QString usage = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/usagetracker.cpp"));
    const QString store = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/settingsstore.cpp"));
    check(!win.isEmpty() && !usage.isEmpty() && !store.isEmpty(), "the sources are readable");

    // every save either checks its result or goes through the helper that checks for it
    int bare = 0, checked = 0;
    for (const QString &line : win.split(QLatin1Char('\n'))) {
        const QString t = line.trimmed();
        if (!t.contains(QStringLiteral("m_store->save(")))
            continue;
        if (t.startsWith(QStringLiteral("if (!m_store->save(")))
            ++checked;
        else
            ++bare;
    }
    std::printf("  saves: %d checked inline, %d unchecked\n", checked, bare);
    check(bare == 0, "no settings save discards its result");
    check(win.contains(QStringLiteral("bool MainWindow::persistSettings")),
          "a single helper reports save failures");
    check(win.contains(QStringLiteral("could not save %s")),
          "and logs them as well as showing them");

    check(usage.contains(QStringLiteral("could not create the history directory")),
          "a history directory that cannot be created is reported");
    check(store.contains(QStringLiteral("could not restrict its permissions")),
          "an unprotected migrated file is reported and removed");

    std::printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
                g_failures == 0 ? "test_silent_failures passed." : "FAILED.");
    return g_failures == 0 ? 0 : 1;
}
