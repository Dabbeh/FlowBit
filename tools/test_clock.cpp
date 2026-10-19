// Verifies the forgiving time parser against the forms a person actually types.
//
// This exists because the previous parser accepted only "H:MM" and "HHMM", and a
// rejected time made a schedule window silently never match. The cases below are
// the ones the user reported wanting ("21" without ":00", 12-hour forms).

#include "schedule.h"

#include <QCoreApplication>
#include <QTextStream>

using namespace net;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    struct Case {
        const char *text;
        const char *expect;   // canonical HH:MM, or "" for invalid
    };

    const Case cases[] = {
        // the forms the user asked for
        {"21", "21:00"},
        {"9", "09:00"},
        {"21:00", "21:00"},
        {"21:30", "21:30"},
        {"9:30", "09:30"},
        {"2130", "21:30"},
        {"930", "09:30"},
        {"9pm", "21:00"},
        {"9PM", "21:00"},
        {"9 pm", "21:00"},
        {"9:30pm", "21:30"},
        {"9:30 pm", "21:30"},
        {"9.30pm", "21:30"},
        {"12am", "00:00"},
        {"12pm", "12:00"},
        {"12:30am", "00:30"},
        {"12:30pm", "12:30"},
        {"1am", "01:00"},
        {"11:59pm", "23:59"},
        {"00:00", "00:00"},
        {"0", "00:00"},
        {"  8pm  ", "20:00"},
        {"8:00", "08:00"},
        {"0800", "08:00"},
        {"20:00", "20:00"},

        // things that must be rejected rather than silently misread
        {"", ""},
        {"banana", ""},
        {"25:00", ""},
        {"24:00", ""},
        {"13pm", ""},
        {"0pm", ""},
        {"9:75", ""},
        {"-1", ""},
        {"9:3:2", ""},
        {"99999", ""},
    };

    int failures = 0;
    int checks = 0;
    for (const Case &c : cases) {
        ++checks;
        const QTime t = parseClock(QString::fromLatin1(c.text));
        const QString got = formatClock(t);
        const QString want = QString::fromLatin1(c.expect);
        if (got != want) {
            ++failures;
            out << QStringLiteral("  FAIL  \"%1\" -> \"%2\"  expected \"%3\"\n")
                       .arg(QLatin1String(c.text), got, want);
        }
    }

    out << QStringLiteral("%1 checks, %2 failures\n").arg(checks).arg(failures);
    if (failures == 0)
        out << QStringLiteral("test_clock passed.\n");
    return failures == 0 ? 0 : 1;
}
