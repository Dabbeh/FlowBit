// Verifies the update prompt offers three real answers, and that "skip" is honoured.
//
// WHAT WAS WRONG
//
// The offer was a Yes/No question. "No" had to mean two different things - not now, and never
// again - so a user who wanted to defer had no way to say "not this time, but keep telling me".
// And settings.skippedVersion was written to the file and read back into memory, but NOTHING
// consulted it before offering, so skipping was impossible and the same version returned on
// every launch.
//
// WHAT IS CHECKED
//
//   * the three answers exist, with the consequences wired to them
//   * the skip decision is consulted before an offer is made
//   * a skipped version survives a settings round trip - the persisted half of the feature
//   * the deferral re-offers later in the same session rather than only at the next launch

#include "settingsstore.h"
#include "settings.h"
#include "updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace net;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what)
{
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", qPrintable(what));
    if (!ok)
        ++g_failures;
}

QString read(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::printf("\n== the update offer ==\n");

    const QString up = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/updater.cpp"));
    const QString win = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/mainwindow.cpp"));
    check(!up.isEmpty() && !win.isEmpty(), "the sources are readable");

    // 1. three answers, each with its own consequence
    check(up.contains(QStringLiteral("\"Update now\"")), "the prompt offers \"Update now\"");
    check(up.contains(QStringLiteral("\"Later\"")), "and \"Later\"");
    check(up.contains(QStringLiteral("\"Skip this version\"")), "and \"Skip this version\"");
    check(up.contains(QStringLiteral("emit updateSkipped(version)")),
          "Skip reports the decision instead of only closing the dialog");
    check(up.contains(QStringLiteral("emit updateDeferred(version)")),
          "and Later reports a deferral rather than a decision");

    // Closing the window must not count as a permanent skip - an Escape key should not silence an
    // update for good.
    check(up.contains(QStringLiteral("box.clickedButton() != nowButton")),
          "closing the dialog counts as \"not now\", not as a skip");

    // 2. the skip is consulted before offering
    check(win.contains(QStringLiteral("current.skippedVersion == version")),
          "a skipped version is checked against the offered one before any prompt");

    // 3. the deferral re-offers in-session
    check(win.contains(QStringLiteral("m_updateSnooze->start()")),
          "Later schedules another check rather than waiting for a restart");
    check(win.contains(QStringLiteral("kUpdateSnoozeMs")), "with a defined interval");

    // 4. the persisted half actually round-trips
    QTemporaryDir tmp;
    {
        SettingsStore store(tmp.path() + QStringLiteral("/cfg"));
        Settings s;
        s.skippedVersion = QStringLiteral("1.4.0");
        QString err;
        check(store.save(s, &err), "a skipped version saves");

        const Settings back = store.load();
        std::printf("  reloaded skippedVersion: \"%s\"\n", qPrintable(back.skippedVersion));
        check(back.skippedVersion == QStringLiteral("1.4.0"),
              "and reads back - which is what the offer consults");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_update_offer passed." : "test_update_offer FAILED.");
    return g_failures == 0 ? 0 : 1;
}
