#include "diagnosticspanel.h"

#include "theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QFontMetrics>
#include <QGridLayout>
#include <QVBoxLayout>

#include <windows.h>

namespace net {

namespace {

// A short coloured marker plus the reading. The marker is a glyph rather than a
// coloured row background so the panel still reads correctly in both themes, and so
// the monochrome chrome is not disturbed.
struct Marker {
    const char *glyph;
    const char *label;
};

Marker markerFor(DiagnosticItem::Status s)
{
    switch (s) {
    case DiagnosticItem::Status::Ok:
        return {"\u2713", "OK"};
    case DiagnosticItem::Status::Info:
        return {"\u00B7", "INFO"};
    case DiagnosticItem::Status::Warning:
        return {"!", "WARNING"};
    case DiagnosticItem::Status::Problem:
        return {"\u2717", "PROBLEM"};
    }
    return {"\u00B7", "INFO"};
}

QColor colourFor(DiagnosticItem::Status s)
{
    switch (s) {
    case DiagnosticItem::Status::Ok:
        return theme::kGreen();
    case DiagnosticItem::Status::Info:
        return theme::kTextMuted();
    case DiagnosticItem::Status::Warning:
        return theme::kAmber();
    case DiagnosticItem::Status::Problem:
        return theme::kRed();
    }
    return theme::kTextMuted();
}

bool processIsElevated()
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

} // namespace

DiagnosticsPanel::DiagnosticsPanel(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    // Preferred vertically, NOT Minimum.
    //
    // Minimum would make this widget's sizeHint a hard floor. The report's sizeHint is
    // large, so that turned into a 1215px minimum on the panel and a 1287px minimum on
    // the card that holds it - inside a card only 806px tall. The card was then placed
    // below the visible content and never drawn, which is the empty box the screenshot
    // showed. Preferred takes the space when it is offered and gives it back when it is
    // not; the page scrolls, so nothing becomes unreachable.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    // -- header: summary and actions ---------------------------------------
    auto *head = new QHBoxLayout();
    head->setSpacing(8);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("statValue"));
    head->addWidget(m_summary);
    head->addStretch(1);

    m_recheck = new QPushButton(QStringLiteral("Re-check"), this);
    m_recheck->setObjectName(QStringLiteral("ghost"));
    m_recheck->setCursor(Qt::PointingHandCursor);
    m_recheck->setToolTip(QStringLiteral("Read the system state again"));
    connect(m_recheck, &QPushButton::clicked, this, &DiagnosticsPanel::refresh);
    head->addWidget(m_recheck);

    m_copy = new QPushButton(QStringLiteral("Copy"), this);
    m_copy->setObjectName(QStringLiteral("ghost"));
    m_copy->setCursor(Qt::PointingHandCursor);
    m_copy->setToolTip(QStringLiteral("Copy the full report to the clipboard"));
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_report.toText());
        emit exported(true, QStringLiteral("Report copied to the clipboard."));
    });
    head->addWidget(m_copy);

    m_export = new QPushButton(QStringLiteral("Save Report"), this);
    m_export->setObjectName(QStringLiteral("ghost"));
    m_export->setCursor(Qt::PointingHandCursor);
    connect(m_export, &QPushButton::clicked, this, [this] {
        // Ask where to save.
        //
        // This used to write straight to the Desktop and open the folder, without
        // asking - which is both surprising and unhelpful if you want the report
        // somewhere specific. A save dialog is the expected behaviour for a button
        // labelled "Save Report".
        const QString suggested =
            QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                .filePath(QStringLiteral("flowbit-diagnostics-%1.txt")
                              .arg(QDateTime::currentDateTime().toString(
                                  QStringLiteral("yyyy-MM-dd-HHmm"))));

        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save diagnostics report"), suggested,
            QStringLiteral("Text files (*.txt);;All files (*)"));
        if (path.isEmpty())
            return;   // cancelled: say nothing and change nothing

        QString error;
        if (exportDiagnostics(m_report, path, &error)) {
            emit exported(true, QStringLiteral("Saved to %1").arg(path));
            // Reveal where it went rather than opening the file: the report is
            // meant to be attached to a message or pasted.
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        } else {
            emit exported(false, QStringLiteral("Could not save the report: %1").arg(error));
        }
    });
    head->addWidget(m_export);

    root->addLayout(head);

    // A grid rather than a stack of rows: the marker, the name and the value each get
    // a column, so names line up and every value starts at the same x. A row-per-layout
    // arrangement let the value absorb the spare width, which left the readings ragged
    // in a wide window and wrapped the names in a narrow one.
    m_rowsGrid = new QGridLayout();
    m_rowsGrid->setContentsMargins(0, 0, 0, 0);
    m_rowsGrid->setHorizontalSpacing(8);
    m_rowsGrid->setVerticalSpacing(8);
    m_rowsGrid->setColumnStretch(0, 0);   // marker
    m_rowsGrid->setColumnStretch(1, 0);   // name
    m_rowsGrid->setColumnStretch(2, 1);   // value
    root->addLayout(m_rowsGrid);

    refresh();
}

void DiagnosticsPanel::setMonitorError(const QString &error)
{
    m_monitorError = error;
}

void DiagnosticsPanel::setMonitorTelemetry(quint64 seen, quint64 decoded, quint64 udp,
                                          int monitorSeconds)
{
    m_seen = seen;
    m_decoded = decoded;
    m_udp = udp;
    m_monitorSeconds = monitorSeconds;
}

void DiagnosticsPanel::setEventIds(const QString &ids)
{
    m_eventIds = ids;
}

void DiagnosticsPanel::setMonitorRunning(bool running)
{
    m_monitorRunning = running;
}

void DiagnosticsPanel::setEventLayout(const QString &layout)
{
    m_eventLayout = layout;
}

void DiagnosticsPanel::setAddressMiss(const QString &text)
{
    m_addressMiss = text;
}

void DiagnosticsPanel::setRawEvents(const QString &text)
{
    m_rawEvents = text;
}

void DiagnosticsPanel::setRawBits(const QString &text)
{
    m_rawBits = text;
}

void DiagnosticsPanel::refresh()
{
    m_report = collectDiagnostics(processIsElevated(), m_monitorError, m_seen, m_decoded, m_udp,
                                  m_monitorRunning, m_monitorSeconds, m_eventLayout, m_addressMiss,
                                  m_rawEvents, m_rawBits, m_eventIds);
    buildRows();
}

QString DiagnosticsPanel::summaryText() const
{
    const int problems = m_report.countOf(DiagnosticItem::Status::Problem);
    const int warnings = m_report.countOf(DiagnosticItem::Status::Warning);

    if (problems == 0 && warnings == 0)
        return QStringLiteral("Everything checked out");
    if (problems == 0)
        return QStringLiteral("%1 warning%2, nothing broken")
            .arg(warnings)
            .arg(warnings == 1 ? QString() : QStringLiteral("s"));
    return QStringLiteral("%1 problem%2 found")
        .arg(problems)
        .arg(problems == 1 ? QString() : QStringLiteral("s"));
}

void DiagnosticsPanel::buildRows()
{
    // Clear the previous rows. Deleted immediately rather than via deleteLater(),
    // so a re-check cannot leave the old readings visible underneath the new ones.
    // Clear the previous rows, layout items included. A grid keeps its items until
    // they are taken, so a re-check would otherwise stack the new readings under the
    // old ones.
    while (QLayoutItem *item = m_rowsGrid->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->setParent(nullptr);
            delete w;
        }
        delete item;
    }

    m_summary->setText(summaryText());
    m_summary->setStyleSheet(
        m_report.hasProblems()
            ? QStringLiteral("color: %1;").arg(theme::kRed().name())
            : (m_report.countOf(DiagnosticItem::Status::Warning) > 0
                   ? QStringLiteral("color: %1;").arg(theme::kAmber().name())
                   : QStringLiteral("color: %1;").arg(theme::kGreen().name())));

    QString currentGroup;

    // The name column is sized to the widest name, so it never clips and never wastes
    // space. Measured from the label font rather than assumed.
    const QFontMetrics nameMetrics(font());
    int nameWidth = 0;
    for (const DiagnosticItem &item : m_report.items)
        nameWidth = qMax(nameWidth, nameMetrics.horizontalAdvance(item.name));
    // A little slack, and a floor so a short report still looks deliberate.
    nameWidth = qBound(140, nameWidth + 8, 320);

    int row = 0;
    for (const DiagnosticItem &item : m_report.items) {
        // A group heading, so related readings stay together. It spans the row, which
        // is what marks it as a heading rather than another reading.
        if (item.group != currentGroup) {
            currentGroup = item.group;
            auto *groupLabel = new QLabel(currentGroup.toUpper(), this);
            groupLabel->setObjectName(QStringLiteral("cardTitle"));
            groupLabel->setContentsMargins(0, row == 0 ? 0 : 10, 0, 0);
            m_rowsGrid->addWidget(groupLabel, row, 0, 1, 3);
            ++row;
        }

        const Marker marker = markerFor(item.status);
        const QColor colour = colourFor(item.status);

        auto *mark = new QLabel(QString::fromUtf8(marker.glyph), this);
        mark->setStyleSheet(QStringLiteral("color: %1;").arg(colour.name()));
        mark->setFixedWidth(14);
        mark->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        mark->setToolTip(QString::fromLatin1(marker.label));
        m_rowsGrid->addWidget(mark, row, 0, Qt::AlignTop);

        auto *name = new QLabel(item.name, this);
        name->setObjectName(QStringLiteral("sub"));
        // Fixed to the measured column width, so the name never wraps and every value
        // below starts at the same x.
        name->setFixedWidth(nameWidth);
        name->setWordWrap(false);
        name->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_rowsGrid->addWidget(name, row, 1, Qt::AlignTop);

        auto *value = new QLabel(item.value, this);
        value->setStyleSheet(QStringLiteral("color: %1;").arg(colour.name()));
        // The value takes the remaining width and wraps there, rather than stretching
        // a row and pushing the reading away from its name.
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        // Deliberately NOT HeightForWidth.
        //
        // A wrapped label answers heightForWidth against whatever width it currently
        // has, and during construction that is a few hundred pixels - so it reported
        // 32px for a single line that fits in 16. Once the grid's own minimum was
        // derived from those numbers the panel demanded 1215px and was scrolled out of
        // sight. Keeping the policy Preferred lets the label wrap when drawn while the
        // layout sizes the row by its single-line height.
        m_rowsGrid->addWidget(value, row, 2);
        ++row;

        if (!item.detail.isEmpty()) {
            auto *detail = new QLabel(item.detail, this);
            detail->setObjectName(QStringLiteral("sub"));
            detail->setWordWrap(true);
            // Indented under the value, so it reads as an explanation of that reading.
            detail->setContentsMargins(0, 1, 0, 0);
            // Selectable so a fix command can be copied without retyping it.
            detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
            m_rowsGrid->addWidget(detail, row, 2);
            ++row;
        }
    }

    // Turn the grid's preferred height into a floor, so a surrounding layout with spare
    // space cannot squeeze the rows down to a sliver.
    // No size constraint on the grid.
    //
    // A constraint would promote the children's size hints - which for a wrapped label
    // are measured against the wrong width - into a minimum for the whole panel. The
    // panel's own Minimum policy is enough to stop it being squeezed; asking for more
    // than the contents need pushed the card below the visible area.
    if (layout())
        layout()->activate();
    updateGeometry();
}

} // namespace net
