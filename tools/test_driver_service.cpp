// Verifies the driver service is treated as WinDivert actually behaves.
//
// RESEARCH (docs/plan-driver-service.md): Windows reports StartMode=Disabled and State=Running
// for the same driver at the same time, because WinDivert registers its service with a disabled
// start type and loads the driver through its own route. A service the SCM considers disabled
// cannot be started by the SCM at all, so a loaded driver proves that route is in use.
//
// Three rules follow, and all three are asserted here because each was previously wrong:
//
//   1. DISABLED + loaded is INFORMATION, not a warning - and the text says why.
//   2. The repair never stops a loaded driver: stopping interrupts shaping and lets a pending
//      deletion complete at an arbitrary moment.
//   3. Nothing deletes the service any more. `sc delete` is what set DeleteFlag in the first
//      place, and it never completes while Flowbit keeps the driver loaded at shutdown.

#include <QCoreApplication>
#include <QFile>
#include <cstdio>

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
    std::printf("\n== the driver service, as WinDivert really behaves ==\n");

    const QString diag = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/diagnostics.cpp"));
    const QString eng = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/src/throttleengine.cpp"));
    const QString rem = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/installer/flowbit-remove-driver.ps1"));
    const QString iss = read(QStringLiteral("C:/Users/enaya/Desktop/Flowbit/installer/flowbit.iss"));
    check(!diag.isEmpty() && !eng.isEmpty() && !rem.isEmpty() && !iss.isEmpty(),
          "the sources are readable");

    // 1. DISABLED + loaded is information
    check(diag.contains(QStringLiteral("(disabled ? DiagnosticItem::Status::Info")),
          "a disabled start type is Info when the driver is loaded");
    check(diag.contains(QStringLiteral("WinDivert registers its own driver service")),
          "and the text explains who set the value and why the driver still runs");
    check(!diag.contains(QStringLiteral("after a restart Windows will refuse to load it")),
          "the claim that a restart would refuse to load it is gone - a restart loads it fine");

    // 2. the repair does not stop a loaded driver
    const int loadedCheck = eng.indexOf(QStringLiteral("if (readDriverServiceState().running)"));
    const int stopStep = eng.indexOf(QStringLiteral("// Step 2: bring the loaded driver out of kernel memory."));
    check(loadedCheck > 0 && stopStep > loadedCheck,
          "the repair returns before reaching the stop step when the driver is loaded");
    check(eng.contains(QStringLiteral("A loaded driver is a working driver")),
          "and says so where the decision is made");

    // 3. nothing deletes the service
    check(!rem.contains(QStringLiteral("delete $name")) && !rem.contains(QStringLiteral("delete WinDivert")),
          "the uninstaller no longer deletes the service");

    // 4. identical driver files are not rewritten
    //
    // Spacing-agnostic: the .iss aligns its columns, so matching on one exact space failed the
    // moment the line was reflowed. The entry is located by filename and then inspected.
    for (const char *name : {"WinDivert.dll\"", "WinDivert64.sys\""}) {
        const int at = iss.indexOf(QString::fromLatin1(name));
        check(at > 0, QStringLiteral("%1 is still installed").arg(QString::fromLatin1(name)));
        if (at > 0) {
            const QString entry = iss.mid(at, 220);
            check(entry.contains(QStringLiteral("onlyifdoesntexist")),
                  QStringLiteral("%1 is not rewritten when already present")
                      .arg(QString::fromLatin1(name)));
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    std::printf("%s\n", g_failures == 0 ? "test_driver_service passed."
                                        : "test_driver_service FAILED.");
    return g_failures == 0 ? 0 : 1;
}
