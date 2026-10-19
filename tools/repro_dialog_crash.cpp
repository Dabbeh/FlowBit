// Reproduces the crash by constructing the real AppRuleDialog, which is what the
// app does when the user presses "Add application" in the App Limiter.
//
// The minidump resolves the fault to appruledialog.o + 0x110, inside
// QHash::findOrInsert<QString> - the insertion path of QSet<QString>. Two
// QSet<QString> insertions exist in that translation unit:
//
//   runningApplications()      - dedupes processes as it enumerates them
//   AppRuleDialog constructor  - dedupes running + installed applications
//
// Calling runningApplications() alone did not crash, so this exercises the
// constructor, which additionally calls installedApplications() (registry
// enumeration) and fixes up the limits.

#include "appruledialog.h"

#include <QApplication>
#include <QTextStream>

using namespace net;

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QTextStream out(stdout);

    // 1. The installed-applications enumeration on its own.
    const QVector<AppCandidate> installed = installedApplications();
    out << QStringLiteral("installed applications from App Paths: %1\n").arg(installed.size());

    // 2. The real dialog, constructed and destroyed repeatedly - this is the exact
    //    code path that crashed, including both QSet<QString> insertions.
    for (int i = 0; i < 12; ++i) {
        AppRule rule;
        rule.id = 100 + quint64(i);
        rule.name = QStringLiteral("Test");
        rule.exe = QStringLiteral("firefox.exe");
        rule.path = QStringLiteral("C:/Program Files/Mozilla Firefox/firefox.exe");
        rule.down = 40 * 1024;
        rule.up = 20 * 1024;

        // Alternate between "new" (clean rule) and "edit" (populated rule), because
        // they take different branches in the constructor.
        AppRuleDialog *dlg = new AppRuleDialog(rule, i % 2 == 0, nullptr);
        out << QStringLiteral("dialog %1 constructed\n").arg(i + 1);
        dlg->deleteLater();

        // Let deleteLater actually run, so destruction order is exercised too.
        QCoreApplication::processEvents();
    }

    out << QStringLiteral("SURVIVED\n");
    return 0;
}
