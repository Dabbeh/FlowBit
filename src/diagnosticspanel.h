#pragma once

#include "diagnostics.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QGridLayout;
class QVBoxLayout;

namespace net {

// Renders a DiagnosticReport as a readable checklist.
//
// The presentation is deliberately status-first: the count of problems appears at
// the top, and each reading carries a coloured marker plus its explanation. A plain
// list of values would leave the user to work out which line matters, which is the
// problem this panel exists to solve.
class DiagnosticsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit DiagnosticsPanel(QWidget *parent = nullptr);

    // Re-reads the system and rebuilds the rows.
    void refresh();

    // The report currently displayed. Exposed so tests can assert on the collected
    // readings without re-running the collection.
    const DiagnosticReport &report() const { return m_report; }

    // Forwards the application monitor's own last failure reason into the report,
    // so the panel and the activity panel never describe one fault two ways.
    void setMonitorError(const QString &error);

    // Forwards the monitor's ETW counters into the report (received vs decoded).
    void setMonitorTelemetry(quint64 seen, quint64 decoded, quint64 udp, int monitorSeconds);
    void setEventIds(const QString &ids);
    void setMonitorRunning(bool running);
    void setEventLayout(const QString &layout);
    void setAddressMiss(const QString &text);
    void setRawEvents(const QString &text);
    void setRawBits(const QString &text);

    // True when the last collection found at least one Problem.
    bool hasProblems() const { return m_report.hasProblems(); }

    // The status shown in the header summary, for tests.
    QString summaryText() const;

signals:
    // Raised after an export attempt so the page can report the outcome.
    void exported(bool ok, const QString &message);

private:
    void buildRows();

    DiagnosticReport m_report;
    QString m_monitorError;
    quint64 m_seen = 0;
    quint64 m_udp = 0;
    QString m_eventIds;
    int m_monitorSeconds = 0;
    quint64 m_decoded = 0;
    bool m_monitorRunning = false;
    QString m_eventLayout;
    QString m_addressMiss;
    QString m_rawEvents;
    QString m_rawBits;
    QGridLayout *m_rowsGrid = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_export = nullptr;
    QPushButton *m_recheck = nullptr;
};

} // namespace net
