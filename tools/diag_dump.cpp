// Writes the diagnostics report for the current machine to a file, so the real
// output can be inspected without launching the GUI.
//
// Usage: diag_dump.exe [output-path]
#include "../src/diagnostics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <windows.h>

#include <cstdio>

using namespace net;

static bool reallyElevated()
{
    BOOL admin = FALSE;
    PSID admins = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &admins)) {
        CheckTokenMembership(nullptr, admins, &admin);
        FreeSid(admins);
    }
    return admin == TRUE;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const DiagnosticReport report = collectDiagnostics(reallyElevated());
    const QString text = report.toText();

    // Also write it to a file, exercising the same path the UI uses.
    QString path = (argc > 1) ? QString::fromLatin1(argv[1]) : diagnosticExportPath();
    QString error;
    const bool ok = exportDiagnostics(report, path, &error);

    std::printf("%s\n", qPrintable(text));
    std::printf("\nexport: %s\n", ok ? qPrintable(path) : qPrintable(error));
    return 0;
}
