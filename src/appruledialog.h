#pragma once

#include "settings.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

namespace net {

// A snapshot of one installed or running application, used by the picker.
struct AppCandidate {
    QString name;        // friendly name
    QString path;        // full executable path
    QString exe;         // file name
    bool running = false;
};

// Enumerates applications the user can choose from.
//
// Two sources, per the brief: processes that are running now, and applications
// registered with the shell. Both are best-effort - a user can always browse for an
// executable instead, which is why the picker never depends on discovery finding
// anything.
QVector<AppCandidate> runningApplications();
QVector<AppCandidate> installedApplications();

// "Add Application" / "Edit Application".
//
// The same dialog serves both, so the fields and validation cannot drift apart.
class AppRuleDialog : public QDialog
{
    Q_OBJECT

public:
    // `rule` is edited in place when `isNew` is false, preserving its id so an edit
    // can never create a duplicate entry.
    AppRuleDialog(const AppRule &rule, bool isNew, QWidget *parent = nullptr);

    // The edited rule. Undefined until exec() returns Accepted.
    AppRule rule() const { return m_rule; }

    // Empty when the fields are valid; otherwise the reason, shown inline.
    QString validationError() const;

signals:
    void accepted();

private:
    void buildUi();
    void refreshCandidateList(const QString &filter);
    void chooseCandidate(int row);
    void browseForExecutable();
    void applyAndAccept();

    // Translates the limit widgets into a RateBps. 0 means unlimited.
    RateBps limitFrom(QComboBox *unit, QLineEdit *value) const;
    void setLimit(QComboBox *unit, QLineEdit *value, RateBps rate);

    AppRule m_rule;
    bool m_isNew = true;

    QLineEdit *m_nameEdit = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_pathLabel = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_candidates = nullptr;

    QLineEdit *m_downValue = nullptr;
    QComboBox *m_downUnit = nullptr;
    QLineEdit *m_upValue = nullptr;
    QComboBox *m_upUnit = nullptr;

    QCheckBox *m_enabled = nullptr;
    QLabel *m_error = nullptr;
    QPushButton *m_ok = nullptr;

    QVector<AppCandidate> m_all;
};

} // namespace net
