#pragma once

#include "schedule.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

namespace net {

class DayPicker;

// Create-or-edit dialog for a single schedule.
//
// The same dialog serves both actions; edit mode simply seeds the fields from the
// supplied rule and preserves its id, which is what stops an edit from producing
// a duplicate.
//
// Validation is done here and reported inline rather than by disabling the Save
// button, so the user can always see why a rule is not acceptable.
class ScheduleDialog : public QDialog
{
    Q_OBJECT

public:
    // `rule` is the rule to edit. When `isNew` is true a fresh id is assigned on
    // acceptance instead of reusing the supplied one.
    // `siblings` are the application's OTHER schedule windows, which become selectable in
    // the Existing Schedules dropdown. Pass them and the dialog gains the selector, the
    // delete control and "+ Add Schedule"; pass none and it is the plain form, which is
    // what a first-time schedule needs.
    //
    // `rule` is the window to show first: the one being edited, or a blank default when
    // the application has none.
    explicit ScheduleDialog(const Rule &rule, bool isNew, QWidget *parent = nullptr,
                            const QVector<Rule> &siblings = {});

    // The window the user settled on. Its id says which: an id that came from `siblings`
    // or the initial rule means an edit, and a fresh id means a new one.
    Rule result() const { return m_result; }

    // Windows created with "+ Add Schedule" during this dialog's lifetime, in order.
    QVector<Rule> addedRules() const { return m_added; }

    // Ids the user deleted. The caller removes exactly these, so deleting one window
    // leaves the others in place.
    QVector<quint64> removedIds() const { return m_removed; }

    // Valid after exec() returns Accepted.

    // Field accessors for tests, so validation can be exercised without driving
    // the widgets through the UI.
    QLineEdit *nameEdit() const { return m_name; }
    QLineEdit *fromEdit() const { return m_from; }
    QLineEdit *toEdit() const { return m_to; }
    DayPicker *dayPicker() const { return m_days; }
    QDoubleSpinBox *downSpin() const { return m_down; }
    QDoubleSpinBox *upSpin() const { return m_up; }
    QComboBox *downUnitBox() const { return m_downUnit; }
    QComboBox *upUnitBox() const { return m_upUnit; }
    QCheckBox *enabledBox() const { return m_enabled; }
    QLabel *errorLabel() const { return m_error; }

    // Runs validation and reports the problem, or an empty string when valid.
    // Builds a Rule from the fields as they stand. Shared by accept() and the Add button
    // so the two cannot convert units differently.
    Rule buildRule() const;

    QString validationError() const;

    // Live feedback for the time fields: normalises a valid pair and
    // calls out an unrecognised time immediately, instead of letting it
    // fail silently at save time.
    void refreshTimeFeedback();

private:
    void accept() override;
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

    // True while the form holds changes that are not stored in m_choices.
    bool hasUnsavedChanges() const;

    // Asks before discarding edits. True when it is safe to carry on.
    bool confirmDiscard();

    // Fills the form from a window, and stores the form's edits back into m_choices.
    void loadWindow(const Rule &rule);
    void storeCurrentEdits();

    // Rebuilds the dropdown from m_choices and shows or hides the section around it.
    void refreshSelector();

    // Handles the dropdown changing, by index into m_choices.
    void selectWindow(int index);

    // Starts a new window without closing: create mode.
    void addWindow();

    // Removes the window on screen, keeping the others.
    void deleteCurrent();

    // Puts the dialog into the "this application has no schedules" state, which is reached
    // by deleting the last window and is the only way to express that in the UI.
    //
    // Without it there was no reachable state meaning "none": deleting the last window fell
    // back to the create form, so Save rebuilt a window and Cancel discarded the deletion.
    // An application therefore could not be taken off its schedule at all.
    void enterNoSchedules();

    // Enables or disables the per-window fields without touching the dialog's buttons.
    void setFieldsEnabled(bool on);

    void onUnitChanged(bool download);

    QLineEdit *m_name = nullptr;
    QLineEdit *m_from = nullptr;
    QLineEdit *m_to = nullptr;
    QLabel *m_timeFeedback = nullptr;
    DayPicker *m_days = nullptr;
    QDoubleSpinBox *m_down = nullptr;
    QDoubleSpinBox *m_up = nullptr;
    QWidget *m_existingSection = nullptr;   // the whole Existing Schedules block
    QComboBox *m_existing = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QComboBox *m_downUnit = nullptr;
    QComboBox *m_upUnit = nullptr;
    QCheckBox *m_enabled = nullptr;
    QLabel *m_error = nullptr;
    // Explains the empty state when the last window has been deleted.
    QLabel *m_noneNote = nullptr;

    Rule m_rule;
    Rule m_result;
    QVector<Rule> m_added;
    QVector<quint64> m_removed;   // windows deleted while this dialog was open

    // Everything the user can move between: the application's windows, plus any added
    // here. m_currentId is the one shown.
    QVector<Rule> m_choices;
    quint64 m_currentId = 0;
    bool m_isNew = true;
    bool m_offerAdd = false;
    bool m_loading = false;
    // True while the dialog is committing "this application has no schedules". Distinct
    // from merely being empty: a first-time dialog is also empty but must create a window.
    bool m_none = false;
    // Physical rate per direction, so switching units re-renders the same rate
    // rather than reinterpreting the digits.
    RateBps m_rate[2] = {kUnlimited, kUnlimited};
};

} // namespace net
