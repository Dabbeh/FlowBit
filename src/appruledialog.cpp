#include "appruledialog.h"

#include "appnames.h"
#include "theme.h"
#include "units.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QVBoxLayout>

#include <windows.h>
#include <tlhelp32.h>

namespace net {

namespace {

// Unit choices, matching the rest of Flowbit's rate editors.
struct UnitChoice {
    const char *label;
    double multiplier;   // to bytes per second
};

const UnitChoice kUnits[] = {
    {"B/s", 1.0},
    {"KB/s", 1024.0},
    {"MB/s", 1024.0 * 1024.0},
};

} // namespace

// ---------------------------------------------------------------------------
// Application discovery
// ---------------------------------------------------------------------------

QVector<AppCandidate> runningApplications()
{
    QVector<AppCandidate> out;
    QSet<QString> seen;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snap, &entry)) {
        do {
            const QString exe = QString::fromWCharArray(entry.szExeFile);
            if (exe.isEmpty())
                continue;

            // Resolve the full path: two unrelated binaries can share a name, and
            // the path is what a rule actually stores.
            QString path;
            HANDLE proc =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (proc) {
                wchar_t buf[MAX_PATH * 4] = {};
                DWORD size = DWORD(sizeof(buf) / sizeof(wchar_t));
                if (QueryFullProcessImageNameW(proc, 0, buf, &size))
                    path = QDir::fromNativeSeparators(QString::fromWCharArray(buf, int(size)));
                CloseHandle(proc);
            }

            if (path.isEmpty())
                continue;   // protected process: nothing useful to add

            const QString key = path.toLower();
            if (seen.contains(key))
                continue;
            seen.insert(key);

            AppCandidate c;
            c.exe = exe;
            c.path = path;
            c.name = friendlyAppName(exe);
            c.running = true;
            out.append(c);
        } while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);

    std::sort(out.begin(), out.end(),
              [](const AppCandidate &a, const AppCandidate &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    return out;
}

QVector<AppCandidate> installedApplications()
{
    QVector<AppCandidate> out;

    // The shell's "App Paths" registry key is the documented place Windows records
    // launchable applications. It is best-effort by design: a miss simply means the
    // user browses for the executable instead.
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths", 0, KEY_READ,
                      &root)
        != ERROR_SUCCESS)
        return out;

    wchar_t nameBuf[512];
    DWORD index = 0;
    DWORD nameLen = 0;
    while (true) {
        nameLen = DWORD(sizeof(nameBuf) / sizeof(wchar_t));
        const LSTATUS rc = RegEnumKeyExW(root, index++, nameBuf, &nameLen, nullptr, nullptr,
                                         nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS)
            break;

        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, nameBuf, 0, KEY_READ, &sub) != ERROR_SUCCESS)
            continue;

        wchar_t value[MAX_PATH * 4] = {};
        DWORD valueLen = DWORD(sizeof(value));
        DWORD type = 0;
        if (RegQueryValueExW(sub, L"", nullptr, &type, reinterpret_cast<LPBYTE>(value), &valueLen)
                == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            QString path = QString::fromWCharArray(value);
            // Some entries carry a command line rather than a bare path.
            const int quoteEnd = path.indexOf(QLatin1Char('"'), 1);
            if (path.startsWith(QLatin1Char('"')))
                path = path.mid(1, quoteEnd > 1 ? quoteEnd - 1 : path.size() - 1);
            path = QDir::fromNativeSeparators(path.trimmed());

            if (path.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)
                && QFileInfo::exists(path)) {
                AppCandidate c;
                c.path = path;
                c.exe = QFileInfo(path).fileName();
                c.name = friendlyAppName(c.exe);
                c.running = false;
                out.append(c);
            }
        }
        RegCloseKey(sub);
    }

    RegCloseKey(root);

    std::sort(out.begin(), out.end(),
              [](const AppCandidate &a, const AppCandidate &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    return out;
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

AppRuleDialog::AppRuleDialog(const AppRule &rule, bool isNew, QWidget *parent)
    : QDialog(parent), m_rule(rule), m_isNew(isNew)
{
    if (m_isNew)
        m_rule = AppRule{};   // start clean; an id is assigned on accept

    setWindowTitle(isNew ? QStringLiteral("Add Application")
                         : QStringLiteral("Edit Application"));
    setModal(true);
    buildUi();
}

void AppRuleDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // ---- chosen application ------------------------------------------------
    auto *chosen = new QHBoxLayout();
    chosen->setSpacing(10);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(32, 32);
    chosen->addWidget(m_iconLabel);

    auto *chosenCol = new QVBoxLayout();
    chosenCol->setSpacing(2);

    m_nameEdit = new QLineEdit(m_rule.name, this);
    m_nameEdit->setPlaceholderText(QStringLiteral("Application name"));
    chosenCol->addWidget(m_nameEdit);

    m_pathLabel = new QLabel(m_rule.path.isEmpty() ? QStringLiteral("No application chosen")
                                                   : m_rule.path,
                             this);
    m_pathLabel->setObjectName(QStringLiteral("sub"));
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    chosenCol->addWidget(m_pathLabel);

    chosen->addLayout(chosenCol, 1);
    root->addLayout(chosen);

    if (!m_rule.path.isEmpty())
        m_iconLabel->setPixmap(appIcon(m_rule.path, m_rule.exe).pixmap(32, 32));

    // ---- picker ------------------------------------------------------------
    auto *pickerBox = new QGroupBox(QStringLiteral("Choose an application"), this);
    auto *pickerLayout = new QVBoxLayout(pickerBox);
    pickerLayout->setSpacing(8);

    auto *searchRow = new QHBoxLayout();
    searchRow->setSpacing(8);

    m_search = new QLineEdit(pickerBox);
    m_search->setPlaceholderText(QStringLiteral("Search applications..."));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this,
            [this](const QString &t) { refreshCandidateList(t); });
    searchRow->addWidget(m_search, 1);

    auto *browse = new QPushButton(QStringLiteral("Browse..."), pickerBox);
    browse->setCursor(Qt::PointingHandCursor);
    browse->setToolTip(QStringLiteral(
        "Choose an executable that does not appear in the list above"));
    connect(browse, &QPushButton::clicked, this, &AppRuleDialog::browseForExecutable);
    searchRow->addWidget(browse);

    pickerLayout->addLayout(searchRow);

    m_candidates = new QListWidget(pickerBox);
    m_candidates->setMinimumHeight(160);
    m_candidates->setIconSize(QSize(20, 20));
    connect(m_candidates, &QListWidget::currentRowChanged, this,
            &AppRuleDialog::chooseCandidate);
    pickerLayout->addWidget(m_candidates, 1);

    root->addWidget(pickerBox, 1);

    // ---- limits ------------------------------------------------------------
    auto *limitsBox = new QGroupBox(QStringLiteral("Bandwidth limits"), this);
    auto *form = new QFormLayout(limitsBox);
    form->setSpacing(8);

    const auto makeLimitRow = [&](const QString &colourArrow, const QColor &accent,
                                  QLineEdit **valueOut, QComboBox **unitOut) {
        auto *row = new QHBoxLayout();
        row->setSpacing(6);

        auto *value = new QLineEdit(limitsBox);
        value->setPlaceholderText(QStringLiteral("Unlimited"));
        value->setMaximumWidth(110);
        row->addWidget(value);

        auto *unit = new QComboBox(limitsBox);
        for (const UnitChoice &u : kUnits)
            unit->addItem(QString::fromLatin1(u.label), u.multiplier);
        row->addWidget(unit);
        row->addStretch(1);

        form->addRow(QStringLiteral("%1 Limit").arg(colourArrow), row);

        // The arrow carries the same cyan/orange language as the graph, so the
        // direction is legible at a glance without reading the label.
        if (QLabel *lbl = qobject_cast<QLabel *>(form->labelForField(row)))
            lbl->setStyleSheet(QStringLiteral("color: %1;").arg(accent.name()));

        *valueOut = value;
        *unitOut = unit;
    };

    makeLimitRow(QStringLiteral("\u2193 Download"), theme::kGraphDownload(), &m_downValue,
                 &m_downUnit);
    makeLimitRow(QStringLiteral("\u2191 Upload"), theme::kGraphUpload(), &m_upValue, &m_upUnit);

    m_enabled = new QCheckBox(QStringLiteral("Enabled"), limitsBox);
    m_enabled->setChecked(m_rule.enabled);
    form->addRow(QString(), m_enabled);

    root->addWidget(limitsBox);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("errorText"));
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    root->addWidget(m_error);

    // ---- buttons -----------------------------------------------------------
    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);

    auto *cancel = new QPushButton(QStringLiteral("Cancel"), this);
    cancel->setObjectName(QStringLiteral("ghost"));
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);

    m_ok = new QPushButton(m_isNew ? QStringLiteral("Add") : QStringLiteral("Save"), this);
    m_ok->setCursor(Qt::PointingHandCursor);
    connect(m_ok, &QPushButton::clicked, this, &AppRuleDialog::applyAndAccept);
    buttons->addWidget(m_ok);

    root->addLayout(buttons);

    // Populate the picker and the existing limits.
    m_all = runningApplications();
    const QVector<AppCandidate> installed = installedApplications();
    QSet<QString> have;
    for (const AppCandidate &c : m_all)
        have.insert(c.path.toLower());
    for (const AppCandidate &c : installed) {
        if (!have.contains(c.path.toLower())) {
            m_all.append(c);
            have.insert(c.path.toLower());
        }
    }
    refreshCandidateList(QString());

    setLimit(m_downUnit, m_downValue, m_rule.down);
    setLimit(m_upUnit, m_upValue, m_rule.up);

    resize(560, 620);
}

void AppRuleDialog::refreshCandidateList(const QString &filter)
{
    m_candidates->clear();

    const QString f = filter.trimmed().toLower();
    for (int i = 0; i < m_all.size(); ++i) {
        const AppCandidate &c = m_all.at(i);
        if (!f.isEmpty() && !c.name.toLower().contains(f) && !c.exe.toLower().contains(f)
            && !c.path.toLower().contains(f))
            continue;

        auto *item = new QListWidgetItem(m_candidates);
        item->setText(c.running ? QStringLiteral("%1  (running)").arg(c.name) : c.name);
        item->setIcon(appIcon(c.path, c.exe));
        item->setToolTip(c.path);
        item->setData(Qt::UserRole, i);
    }
}

void AppRuleDialog::chooseCandidate(int row)
{
    if (row < 0)
        return;
    QListWidgetItem *item = m_candidates->item(row);
    if (!item)
        return;

    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= m_all.size())
        return;

    const AppCandidate &c = m_all.at(index);
    m_rule.path = c.path;
    m_rule.exe = c.exe;
    if (m_nameEdit->text().trimmed().isEmpty() || m_nameEdit->text() == m_rule.name)
        m_nameEdit->setText(c.name);
    m_rule.name = m_nameEdit->text();

    m_pathLabel->setText(c.path);
    m_iconLabel->setPixmap(appIcon(c.path, c.exe).pixmap(32, 32));
    m_error->setVisible(false);

    // A fresh pick clears any earlier validation complaint.
    m_ok->setEnabled(true);
}

void AppRuleDialog::browseForExecutable()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose an application"), QString(),
        QStringLiteral("Programs (*.exe);;All files (*)"));
    if (path.isEmpty())
        return;

    const QString native = QDir::fromNativeSeparators(path);
    m_rule.path = native;
    m_rule.exe = QFileInfo(native).fileName();
    if (m_nameEdit->text().trimmed().isEmpty())
        m_nameEdit->setText(friendlyAppName(m_rule.exe));

    m_pathLabel->setText(native);
    m_iconLabel->setPixmap(appIcon(native, m_rule.exe).pixmap(32, 32));
    m_error->setVisible(false);
}

RateBps AppRuleDialog::limitFrom(QComboBox *unit, QLineEdit *value) const
{
    const QString text = value->text().trimmed();
    if (text.isEmpty())
        return kUnlimited;

    bool ok = false;
    const double number = text.toDouble(&ok);
    if (!ok || number <= 0)
        return kUnlimited;

    return RateBps(number * unit->currentData().toDouble());
}

void AppRuleDialog::setLimit(QComboBox *unit, QLineEdit *value, RateBps rate)
{
    if (rate <= 0) {
        value->clear();
        unit->setCurrentIndex(1);   // KB/s
        return;
    }

    // Present the value in the largest unit that keeps it a whole number, which is
    // how the user is most likely to have entered it.
    double amount = double(rate);
    int unitIndex = 0;
    if (amount >= 1024.0 * 1024.0 && amount / (1024.0 * 1024.0) >= 1.0
        && qFuzzyIsNull(std::fmod(amount, 1024.0 * 1024.0))) {
        amount /= 1024.0 * 1024.0;
        unitIndex = 2;
    } else if (amount >= 1024.0 && qFuzzyIsNull(std::fmod(amount, 1024.0))) {
        amount /= 1024.0;
        unitIndex = 1;
    }

    value->setText(QString::number(amount, 'g', 10));
    unit->setCurrentIndex(unitIndex);
}

QString AppRuleDialog::validationError() const
{
    if (m_rule.path.trimmed().isEmpty() && m_rule.exe.trimmed().isEmpty())
        return QStringLiteral("Choose an application, or browse for its executable.");

    if (!m_rule.path.isEmpty() && !QFileInfo::exists(m_rule.path)) {
        // Not fatal: the application may be on a disconnected drive right now. It
        // is worth saying though, because the rule will not match until it returns.
        return QString();
    }

    const auto badLimit = [](QLineEdit *e) {
        const QString t = e->text().trimmed();
        if (t.isEmpty())
            return false;
        bool ok = false;
        const double v = t.toDouble(&ok);
        return !ok || v < 0;
    };
    if (badLimit(m_downValue))
        return QStringLiteral("The download limit must be a number, or empty for unlimited.");
    if (badLimit(m_upValue))
        return QStringLiteral("The upload limit must be a number, or empty for unlimited.");

    if (limitFrom(m_downUnit, m_downValue) == kUnlimited
        && limitFrom(m_upUnit, m_upValue) == kUnlimited) {
        return QStringLiteral(
            "Set at least one limit. With both directions unlimited this entry would "
            "have no effect.");
    }

    return QString();
}

void AppRuleDialog::applyAndAccept()
{
    m_rule.name = m_nameEdit->text().trimmed();
    if (m_rule.name.isEmpty())
        m_rule.name = m_rule.exe;
    m_rule.down = limitFrom(m_downUnit, m_downValue);
    m_rule.up = limitFrom(m_upUnit, m_upValue);
    m_rule.enabled = m_enabled->isChecked();

    const QString error = validationError();
    if (!error.isEmpty()) {
        // Reported inline rather than by disabling the button, so the user learns
        // what is wrong instead of being left with a dead control.
        m_error->setText(error);
        m_error->setVisible(true);
        return;
    }

    accept();
}

} // namespace net
