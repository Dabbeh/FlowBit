#pragma once

#include <QString>
#include <QVector>

namespace net {

// One diagnostic reading.
//
// `status` drives how the row is presented, because the point of this panel is to
// separate "working" from "configured but not working" from "cannot work at all".
// A single grey text blob would hide exactly the distinction that matters.
struct DiagnosticItem {
    enum class Status {
        Ok,        // working
        Info,      // a fact, neither good nor bad
        Warning,   // works, but something is degraded or worth knowing
        Problem,   // not working, and the user can act on it
    };

    QString group;     // "Driver", "Permissions", "Monitoring", ...
    QString name;      // "WinDivert service"
    QString value;     // the reading, e.g. "disabled (Start = 4)"
    QString detail;    // optional explanation or the fix
    Status status = Status::Info;
};

// A self-contained report, so the panel and the export share one code path and
// cannot disagree with each other.
struct DiagnosticReport {
    QVector<DiagnosticItem> items;

    int countOf(DiagnosticItem::Status s) const;
    bool hasProblems() const { return countOf(DiagnosticItem::Status::Problem) > 0; }

    // Plain text, suitable for a file or for pasting into a message. Deliberately
    // self-describing: it names the build, the OS and each reading, so it is useful
    // without any surrounding context.
    QString toText() const;
};

// Collects every reading. Does not modify anything and needs no privileges: it
// reads state rather than changing it, which is what makes it safe to run.
//
// `wantElevated` reports whether the current process holds Administrator rights.
// Passed in rather than queried here so the report has one consistent view of the
// process, which the caller has already had to determine.
//
// `monitorError` carries the application monitor's own last failure reason, if
// any, so the report and the panel tell one story instead of two.
//
// `eventsSeen` / `eventsDecoded` are the application monitor's ETW counters.
// They separate two entirely different failures: seen == 0 means the provider
// is delivering nothing on this Windows build; seen > 0 with decoded == 0 means
// the payload schema does not match the decoder.
DiagnosticReport collectDiagnostics(bool isElevated,
                                    const QString &monitorError = QString(),
                                    quint64 eventsSeen = 0,
                                    quint64 eventsDecoded = 0,
      quint64 udpEvents = 0,
                                    bool monitorRunning = false,
                                    int monitorSeconds = 0,
                                    const QString &eventLayout = QString(),
                                    const QString &addressMiss = QString(),
                                    const QString &rawEvents = QString(),
                                    const QString &rawBits = QString(),
      const QString &eventIds = QString());

// Where an exported report is written. Exposed so the UI can tell the user before
// writing, rather than after.
QString diagnosticExportPath();

// Writes `report` to `path`. Returns false and sets `error` on failure.
bool exportDiagnostics(const DiagnosticReport &report, const QString &path,
                       QString *error);

} // namespace net
