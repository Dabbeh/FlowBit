#include "scheduledialog.h"

#include "daypicker.h"
#include "theme.h"
#include "units.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace net {

namespace {

// The units a schedule may use. B/s is omitted: a per-second byte limit is never
// a meaningful schedule, and offering it only invites mistakes.
const SpeedUnit kUnits[] = {SpeedUnit::MibPerSec, SpeedUnit::KibPerSec};

QLabel *fieldLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("fieldLabel"));
    return l;
}

} // namespace

ScheduleDialog::ScheduleDialog(const Rule &rule, bool isNew, QWidget *parent,
                               const QVector<Rule> &siblings)
    : QDialog(parent), m_rule(rule), m_isNew(isNew)
{
    // Every window the user can move between.
    //
    // The caller passes the application's OTHER windows; the one being shown has to be
    // added here, or it would be missing from its own list - which is exactly what
    // happened: the dropdown showed only the sibling, so the window the dialog had opened
    // with could not be returned to.
    m_choices = siblings;
    if (rule.id != 0) {
        bool already = false;
        for (const Rule &r : m_choices) {
            if (r.id == rule.id) {
                already = true;
                break;
            }
        }
        if (!already)
            m_choices.prepend(rule);
    }
    m_currentId = rule.id;

    setWindowTitle(isNew ? QStringLiteral("Add Schedule") : QStringLiteral("Edit Schedule"));
    setModal(true);
    setMinimumWidth(400);

    m_loading = true;

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);

    // -- name ---------------------------------------------------------------
    root->addWidget(fieldLabel(QStringLiteral("Rule Name"), this));
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("Night Schedule"));
    m_name->setText(rule.name);
    root->addWidget(m_name);

    // -- existing schedules --------------------------------------------------
    //
    // Directly below Rule Name. Hidden entirely when the application has no windows, so a
    // first-time schedule shows the form exactly as it always did; shown otherwise, which
    // is what lets either of two windows be selected and edited.
    m_existingSection = new QWidget(this);
    {
        auto *section = new QVBoxLayout(m_existingSection);
        section->setContentsMargins(0, 0, 0, 0);
        section->setSpacing(4);

        section->addWidget(fieldLabel(QStringLiteral("Existing Schedules"), m_existingSection));

        auto *row = new QHBoxLayout();
        row->setSpacing(6);

        m_existing = new QComboBox(m_existingSection);
        // Named, so it can be told apart from the speed-unit selectors, which are also
        // QComboBoxes and come earlier in the dialog.
        m_existing->setObjectName(QStringLiteral("existingSchedules"));
        m_existing->setCursor(Qt::PointingHandCursor);
        m_existing->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(m_existing, QOverload<int>::of(&QComboBox::activated), this,
                [this](int index) { selectWindow(index); });
        row->addWidget(m_existing, 1);

        m_delete = new QPushButton(QString::fromUtf8("\u2715"), m_existingSection);
        m_delete->setObjectName(QStringLiteral("deleteSchedule"));
        m_delete->setCursor(Qt::PointingHandCursor);
        m_delete->setFixedWidth(36);
        m_delete->setToolTip(QStringLiteral("Delete this schedule"));
        connect(m_delete, &QPushButton::clicked, this, &ScheduleDialog::deleteCurrent);
        row->addWidget(m_delete);

        section->addLayout(row);
    }
    root->addWidget(m_existingSection);

    // -- times, side by side -------------------------------------------------
    auto *timeRow = new QHBoxLayout();
    timeRow->setSpacing(12);

    auto *fromCol = new QVBoxLayout();
    fromCol->setSpacing(4);
    fromCol->addWidget(fieldLabel(QStringLiteral("Start Time"), this));
    m_from = new QLineEdit(this);
    // Placeholder shows the two most natural styles so neither is a surprise.
    m_from->setPlaceholderText(QStringLiteral("8pm  or  20:00"));
    // No length cap: "9:30 pm" is 7 characters, and the old 5-character limit
    // made every 12-hour form impossible to type at all.
    m_from->setMaxLength(16);
    m_from->setText(rule.from);
    m_from->setToolTip(QStringLiteral(
        "Type a time in whichever form is natural:\n"
        "    8pm        9:30 pm      21     21:00     2130\n"
        "Start typing and it is normalised to HH:MM when it is valid."));
    fromCol->addWidget(m_from);

    auto *toCol = new QVBoxLayout();
    toCol->setSpacing(4);
    toCol->addWidget(fieldLabel(QStringLiteral("End Time"), this));
    m_to = new QLineEdit(this);
    m_to->setPlaceholderText(QStringLiteral("9pm  or  21:00"));
    m_to->setMaxLength(16);
    m_to->setText(rule.to);
    m_to->setToolTip(m_from->toolTip());
    toCol->addWidget(m_to);

    timeRow->addLayout(fromCol, 1);
    timeRow->addLayout(toCol, 1);
    root->addLayout(timeRow);

    // Live feedback: an unparseable time used to fail silently, which meant a
    // schedule simply never activated with nothing on screen to explain it.
    m_timeFeedback = new QLabel(this);
    m_timeFeedback->setObjectName(QStringLiteral("sub"));
    m_timeFeedback->setWordWrap(true);
    root->addWidget(m_timeFeedback);

    // Quick presets: the common windows are one click instead of typing two
    // times, which is the whole reason the dialog felt laborious.
    auto *presetRow = new QHBoxLayout();
    presetRow->setSpacing(6);
    presetRow->addWidget(fieldLabel(QStringLiteral("Quick"), this));
    struct Preset { const char *label; const char *from; const char *to; };
    static const Preset kPresets[] = {
        {"Evening 8-9pm", "20:00", "21:00"},
        {"Night 10pm-7am", "22:00", "07:00"},
        {"Work 9-5", "09:00", "17:00"},
        {"Lunch 12-1pm", "12:00", "13:00"},
    };
    for (const Preset &p : kPresets) {
        auto *b = new QPushButton(QString::fromLatin1(p.label), this);
        b->setObjectName(QStringLiteral("ghost"));
        b->setCursor(Qt::PointingHandCursor);
        const QString from = QString::fromLatin1(p.from);
        const QString to = QString::fromLatin1(p.to);
        connect(b, &QPushButton::clicked, this, [this, from, to] {
            m_from->setText(from);
            m_to->setText(to);
            refreshTimeFeedback();
        });
        presetRow->addWidget(b);
    }
    presetRow->addStretch(1);
    root->addLayout(presetRow);

    // Live validation: normalise as the user types, so a typo is visible at once.
    connect(m_from, &QLineEdit::textChanged, this, [this] { refreshTimeFeedback(); });
    connect(m_to, &QLineEdit::textChanged, this, [this] { refreshTimeFeedback(); });
    refreshTimeFeedback();

    auto *hint = new QLabel(
        QStringLiteral("An end time earlier than the start runs overnight into the next day."),
        this);
    hint->setObjectName(QStringLiteral("sub"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    // -- days ---------------------------------------------------------------
    root->addWidget(fieldLabel(QStringLiteral("Repeat On"), this));
    m_days = new DayPicker(this);
    m_days->setMask(rule.days);
    root->addWidget(m_days, 0, Qt::AlignLeft);

    // -- limits -------------------------------------------------------------
    const auto makeLimit = [&](const QString &caption, RateBps rate, QDoubleSpinBox **spinOut,
                               QComboBox **unitOut, int index) {
        root->addWidget(fieldLabel(caption, this));

        auto *row = new QHBoxLayout();
        row->setSpacing(8);

        auto *spin = new QDoubleSpinBox(this);
        spin->setRange(0.0, 1000000.0);
        spin->setDecimals(2);
        spin->setSingleStep(0.5);
        spin->setKeyboardTracking(false);
        spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        spin->setToolTip(QStringLiteral("0 means unlimited."));
        spin->setMinimumWidth(110);

        auto *unit = new QComboBox(this);
        for (SpeedUnit u : kUnits)
            unit->addItem(speedUnitLabel(u), int(u));
        unit->setMinimumWidth(84);

        const SpeedUnit chosen = rate > 0 ? bestUnitFor(rate) : SpeedUnit::MibPerSec;
        unit->setCurrentIndex(qMax(0, unit->findData(int(chosen))));
        spin->setValue(rate > 0 ? fromBytesPerSec(rate, chosen) : 0.0);
        m_rate[index] = rate;

        row->addWidget(spin, 1);
        row->addWidget(unit, 0);
        root->addLayout(row);

        *spinOut = spin;
        *unitOut = unit;

        connect(unit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, index](int) { onUnitChanged(index == 0); });
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, index, unit](double v) {
                    // Keep the remembered physical rate in step, so a later unit
                    // switch re-renders rather than reinterprets.
                    m_rate[index] = toBytesPerSec(v, SpeedUnit(unit->currentData().toInt()));
                });
    };

    makeLimit(QStringLiteral("Download Limit"), rule.down, &m_down, &m_downUnit, 0);
    makeLimit(QStringLiteral("Upload Limit"), rule.up, &m_up, &m_upUnit, 1);

    // -- enabled ------------------------------------------------------------
    m_enabled = new QCheckBox(QStringLiteral("Enable this schedule"), this);
    m_enabled->setChecked(rule.enabled);
    root->addWidget(m_enabled);

    // -- inline validation message ------------------------------------------
    // Shown only in the empty state, so the consequence of deleting the last window is
    // stated before Save rather than discovered afterwards.
    m_noneNote = new QLabel(
        QStringLiteral("This application has no schedules.\n\n"
                       "Save to remove the one you deleted. It will then be limited all the "
                       "time while it is enabled, instead of only during a window."),
        this);
    m_noneNote->setObjectName(QStringLiteral("sub"));
    m_noneNote->setWordWrap(true);
    m_noneNote->hide();
    root->addWidget(m_noneNote);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("error"));
    m_error->setWordWrap(true);
    m_error->hide();
    root->addWidget(m_error);

    root->addStretch(1);

    // -- actions ------------------------------------------------------------
    //   [+ Add Schedule]                  [Cancel] [Save Rule / Save Changes]
    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(8);

    // Bottom-left, shown once a window exists. It starts another without closing, so a
    // second window needs no second dialog.
    m_addButton = new QPushButton(QStringLiteral("+ Add Schedule"), this);
    m_addButton->setObjectName(QStringLiteral("ghost"));
    m_addButton->setCursor(Qt::PointingHandCursor);
    m_addButton->setToolTip(QStringLiteral("Start another schedule for this application"));
    connect(m_addButton, &QPushButton::clicked, this, &ScheduleDialog::addWindow);
    buttons->addWidget(m_addButton);

    buttons->addStretch(1);

    auto *cancel = new QPushButton(QStringLiteral("Cancel"), this);
    cancel->setObjectName(QStringLiteral("ghost"));
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);

    m_saveButton = new QPushButton(isNew ? QStringLiteral("Save Rule")
                                         : QStringLiteral("Save Changes"),
                                   this);
    m_saveButton->setCursor(Qt::PointingHandCursor);
    m_saveButton->setDefault(true);
    connect(m_saveButton, &QPushButton::clicked, this, &ScheduleDialog::accept);
    buttons->addWidget(m_saveButton);

    root->addLayout(buttons);

    m_loading = false;
    refreshSelector();
}
bool ScheduleDialog::hasUnsavedChanges() const
{
    if (m_loading || !m_from || !m_to || !m_name)
        return false;
    // The empty state carries no window to lose, so closing from it never nags.
    if (m_none)
        return false;
    const Rule now = const_cast<ScheduleDialog *>(this)->buildRule();

    for (const Rule &r : m_choices) {
        if (r.id != m_currentId || m_currentId == 0)
            continue;
        return !(r.name == now.name && r.from == now.from && r.to == now.to
                 && r.days == now.days && r.enabled == now.enabled && r.down == now.down
                 && r.up == now.up);
    }
    const Rule baseline = m_rule;
    return !(baseline.name == now.name && baseline.from == now.from
             && baseline.to == now.to && baseline.days == now.days
             && baseline.enabled == now.enabled && baseline.down == now.down
             && baseline.up == now.up);
}

bool ScheduleDialog::confirmDiscard()
{
    if (!hasUnsavedChanges())
        return true;
    const int answer = QMessageBox::question(
        this, QStringLiteral("Discard changes?"),
        QStringLiteral("This schedule has unsaved changes.\n\nDiscard them?"),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Discard;
}

void ScheduleDialog::storeCurrentEdits()
{
    if (!m_from || m_currentId == 0)
        return;
    Rule edited = buildRule();
    edited.id = m_currentId;
    for (Rule &r : m_choices) {
        if (r.id == m_currentId) {
            r = edited;
            return;
        }
    }
}

void ScheduleDialog::refreshSelector()
{
    if (!m_existingSection || !m_existing || !m_addButton)
        return;

    // Shown only when there is a window to move between. While creating one, or when the
    // application has none, the dropdown would be an empty control.
    const bool show = !m_isNew && !m_choices.isEmpty() && !m_none;
    m_existingSection->setVisible(show);
    // Kept reachable in the empty state, so deleting the last window is still undoable
    // without cancelling and starting over.
    m_addButton->setVisible(!m_choices.isEmpty() || m_none);

    if (!show)
        return;

    const QSignalBlocker block(m_existing);
    m_existing->clear();
    for (const Rule &r : m_choices)
        m_existing->addItem(describeRule(r, -1), QVariant::fromValue<qulonglong>(r.id));

    const int index = m_existing->findData(QVariant::fromValue<qulonglong>(m_currentId));
    if (index >= 0)
        m_existing->setCurrentIndex(index);
}

void ScheduleDialog::loadWindow(const Rule &rule)
{
    // Filling the fields must not look like the user editing them, or switching would
    // report unsaved changes immediately.
    m_loading = true;

    m_rule = rule;
    m_currentId = rule.id;
    m_name->setText(rule.name);
    m_from->setText(rule.from);
    m_to->setText(rule.to);
    if (m_days)
        m_days->setMask(rule.days);
    if (m_enabled)
        m_enabled->setChecked(rule.enabled);

    // Each window carries its own limits, so they are loaded too. A stored rate is shown
    // in whichever unit reads best.
    const auto setRate = [](QDoubleSpinBox *box, QComboBox *unit, RateBps rate) {
        if (!box || !unit)
            return;
        if (rate == kUnlimited) {
            box->setValue(0.0);
            return;
        }
        const double mib = double(rate) / (1024.0 * 1024.0);
        if (mib >= 1.0) {
            const int at = unit->findData(int(SpeedUnit::MibPerSec));
            if (at >= 0)
                unit->setCurrentIndex(at);
            box->setValue(mib);
        } else {
            const int at = unit->findData(int(SpeedUnit::KibPerSec));
            if (at >= 0)
                unit->setCurrentIndex(at);
            box->setValue(double(rate) / 1024.0);
        }
    };
    setRate(m_down, m_downUnit, rule.down);
    setRate(m_up, m_upUnit, rule.up);

    m_loading = false;
    if (m_error)
        m_error->hide();
    refreshTimeFeedback();
}

void ScheduleDialog::selectWindow(int index)
{
    if (!m_existing || index < 0 || index >= m_choices.size())
        return;

    const quint64 wanted = m_existing->itemData(index).value<qulonglong>();
    if (wanted == m_currentId)
        return;

    // Edits in progress are kept, not lost: they are stored back before moving on, so
    // switching between windows cannot discard work. Only an explicit discard asks.
    if (!confirmDiscard()) {
        refreshSelector();   // put the dropdown back where it was
        return;
    }
    storeCurrentEdits();

    for (const Rule &r : m_choices) {
        if (r.id == wanted) {
            loadWindow(r);
            break;
        }
    }
    refreshSelector();
}

void ScheduleDialog::addWindow()
{
    if (!confirmDiscard())
        return;

    // Coming back from the empty state, so the fields and the note go away with it.
    m_none = false;
    if (m_noneNote)
        m_noneNote->hide();
    setFieldsEnabled(true);

    storeCurrentEdits();

    // Create mode. The name is kept - it is the application's name and every window of
    // that application carries it - while the times return to the usual night window and
    // the limits are cleared, so nothing is carried over by accident.
    Rule fresh;
    fresh.id = 0;                 // assigned on accept, which marks it as new
    fresh.name = m_name->text().trimmed().isEmpty() ? m_rule.name : m_name->text().trimmed();
    fresh.from = QStringLiteral("22:00");
    fresh.to = QStringLiteral("07:00");
    fresh.days = 0x7F;
    fresh.down = kUnlimited;
    fresh.up = kUnlimited;
    fresh.enabled = true;

    m_isNew = true;
    loadWindow(fresh);
    setWindowTitle(QStringLiteral("Add Schedule"));
    if (m_saveButton)
        m_saveButton->setText(QStringLiteral("Save Rule"));
    refreshSelector();
}

void ScheduleDialog::deleteCurrent()
{
    if (m_isNew || m_currentId == 0)
        return;

    QString what = QStringLiteral("this schedule");
    for (const Rule &r : m_choices) {
        if (r.id == m_currentId)
            what = describeRule(r, -1);
    }

    const int answer = QMessageBox::question(
        this, QStringLiteral("Delete schedule"),
        QStringLiteral("Delete this schedule?\n\n  %1\n\nOnly this one is removed; the "
                       "application's other schedules are kept.")
            .arg(what),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    const quint64 gone = m_currentId;
    m_removed.append(gone);

    for (int i = 0; i < m_choices.size(); ++i) {
        if (m_choices.at(i).id == gone) {
            m_choices.removeAt(i);
            break;
        }
    }

    if (m_choices.isEmpty()) {
        // The last window went. This used to fall back to the create form, which meant the
        // removal could never be saved: Save rebuilt a window from that form, and Cancel
        // discarded the deletion. The empty state is now a real answer of its own.
        enterNoSchedules();
    } else {
        m_isNew = false;
        loadWindow(m_choices.first());
    }
    refreshSelector();
}

// "This application has no schedules" - reachable, saveable, and distinct from the
// first-time dialog, which is also blank but is meant to create a window.
void ScheduleDialog::enterNoSchedules()
{
    m_none = true;
    m_isNew = false;
    m_currentId = 0;

    m_loading = true;
    m_name->clear();
    m_from->clear();
    m_to->clear();
    if (m_days)
        m_days->setMask(0);
    if (m_down)
        m_down->setValue(0.0);
    if (m_up)
        m_up->setValue(0.0);
    if (m_enabled)
        m_enabled->setChecked(false);
    m_loading = false;

    // Nothing on screen describes a window any more, so the fields are inert rather than
    // showing values that would not be stored.
    setFieldsEnabled(false);

    if (m_timeFeedback) {
        m_timeFeedback->clear();
        m_timeFeedback->setStyleSheet(QString());
    }
    if (m_error)
        m_error->hide();
    if (m_noneNote)
        m_noneNote->show();

    setWindowTitle(QStringLiteral("Remove Schedule"));
    if (m_saveButton)
        m_saveButton->setText(QStringLiteral("Remove Schedule"));
}

void ScheduleDialog::setFieldsEnabled(bool on)
{
    // Deliberately not setEnabled() on the dialog: that would take Save and Cancel with it,
    // and the whole point of this state is that Save is still reachable.
    for (QWidget *w : {static_cast<QWidget *>(m_name), static_cast<QWidget *>(m_from),
                       static_cast<QWidget *>(m_to), static_cast<QWidget *>(m_days),
                       static_cast<QWidget *>(m_down), static_cast<QWidget *>(m_downUnit),
                       static_cast<QWidget *>(m_up), static_cast<QWidget *>(m_upUnit),
                       static_cast<QWidget *>(m_enabled)}) {
        if (w)
            w->setEnabled(on);
    }
}

void ScheduleDialog::reject()
{
    // Cancel passes the same guard as the window's close button, so edits cannot be lost
    // by either route.
    if (!confirmDiscard())
        return;
    QDialog::reject();
}

void ScheduleDialog::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscard()) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}


void ScheduleDialog::onUnitChanged(bool download)
{
    if (m_loading)
        return;

    QDoubleSpinBox *spin = download ? m_down : m_up;
    QComboBox *unit = download ? m_downUnit : m_upUnit;
    const RateBps rate = m_rate[download ? 0 : 1];
    const SpeedUnit u = SpeedUnit(unit->currentData().toInt());

    m_loading = true;
    spin->setValue(rate > 0 ? fromBytesPerSec(rate, u) : 0.0);
    m_loading = false;
}

QString ScheduleDialog::validationError() const
{
    if (m_name->text().trimmed().isEmpty())
        return QStringLiteral("Give the schedule a name.");

    const QTime from = parseClock(m_from->text());
    const QTime to = parseClock(m_to->text());
    if (!from.isValid())
        return QStringLiteral("\"%1\" is not a time I recognise. Try 8pm, 9:30 pm, "
                              "21, 21:00 or 2130.")
            .arg(m_from->text().trimmed());
    if (!to.isValid())
        return QStringLiteral("\"%1\" is not a time I recognise. Try 9pm, 9:30 pm, "
                              "21, 21:00 or 2130.")
            .arg(m_to->text().trimmed());

    // from == to is accepted as the "all day" convention the scheduler already
    // uses, so it is deliberately not rejected here.
    if (m_days->mask() == 0)
        return QStringLiteral("Choose at least one day.");

    return QString();
}

void ScheduleDialog::refreshTimeFeedback()
{
    if (!m_timeFeedback)
        return;

    const QTime from = parseClock(m_from->text());
    const QTime to = parseClock(m_to->text());

    // Only speak when there is something to say: a valid pair is normalised and
    // described, an invalid one is called out immediately rather than at save
    // time. Silence on an invalid time is what made schedules fail invisibly.
    if (!from.isValid() && !m_from->text().trimmed().isEmpty()) {
        m_timeFeedback->setText(
            QStringLiteral("\"%1\" is not a time I recognise - try 8pm, 9:30 pm, 21, "
                           "21:00 or 2130.")
                .arg(m_from->text().trimmed()));
        m_timeFeedback->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kAmber().name()));
        return;
    }
    if (!to.isValid() && !m_to->text().trimmed().isEmpty()) {
        m_timeFeedback->setText(
            QStringLiteral("\"%1\" is not a time I recognise - try 9pm, 9:30 pm, 21, "
                           "21:00 or 2130.")
                .arg(m_to->text().trimmed()));
        m_timeFeedback->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kAmber().name()));
        return;
    }

    if (from.isValid() && to.isValid()) {
        const bool overnight = minutesOfDay(from) > minutesOfDay(to);
        m_timeFeedback->setText(
            QStringLiteral("Runs %1 to %2%3")
                .arg(formatClock(from), formatClock(to),
                     overnight ? QStringLiteral(" (overnight)") : QString()));
        m_timeFeedback->setStyleSheet(QString());
        return;
    }

    m_timeFeedback->clear();
    m_timeFeedback->setStyleSheet(QString());
}

// Builds a Rule from the fields as they stand. Extracted from accept() so the Add button
// can store a window without closing the dialog; one copy means the unit conversion cannot
// differ between saving and adding.
Rule ScheduleDialog::buildRule() const
{
    Rule result;
    result.name = m_name->text().trimmed();

    const QTime from = parseClock(m_from->text());
    const QTime to = parseClock(m_to->text());
    result.from = from.toString(QStringLiteral("HH:mm"));
    result.to = to.toString(QStringLiteral("HH:mm"));
    result.days = m_days->mask();
    result.enabled = m_enabled->isChecked();

    result.down = m_down->value() > 0
                      ? toBytesPerSec(m_down->value(),
                                      SpeedUnit(m_downUnit->currentData().toInt()))
                      : kUnlimited;
    result.up = m_up->value() > 0
                    ? toBytesPerSec(m_up->value(),
                                    SpeedUnit(m_upUnit->currentData().toInt()))
                    : kUnlimited;
    return result;
}

void ScheduleDialog::accept()
{
    // The empty state IS the answer: nothing to validate, and `result()` deliberately stays
    // id 0 so the caller writes no window back. Requiring a valid window here is precisely
    // what made removing the last schedule impossible.
    if (m_none) {
        m_result = Rule{};
        QDialog::accept();
        return;
    }

    const QString problem = validationError();
    if (!problem.isEmpty()) {
        // Report the reason and stay open: no rule is created.
        m_error->setText(problem);
        m_error->show();
        return;
    }

    m_error->hide();

    Rule result = buildRule();
    // Edit mode keeps the existing identity, which is what prevents an edit from
    // creating a duplicate rule.
    result.id = m_isNew ? nextRuleId() : m_rule.id;

    m_result = result;
    QDialog::accept();
}

} // namespace net
