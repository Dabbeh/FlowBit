// Regression suite for the repaired Time Scheduler.
//
// Every check targets a defect that actually existed, or a requirement the brief
// calls out explicitly. The central one is deletion: the old handler located
// rules by position AND required the row to hold keyboard focus, so clicking
// Remove did nothing - the click moved focus to the button, no row qualified as
// "selected", and the handler returned early.
//
// Deletion is exercised through the window's own removeSchedule(), because the
// confirmation dialog would block an offscreen run and the confirmation itself
// was never the broken part.
//
// Build/run: tools/run_scheduler_tests.ps1
#include "../src/mainwindow.h"
#include "../src/schedule.h"
#include "../src/schedulecard.h"
#include "../src/scheduledialog.h"
#include "../src/settingsstore.h"
#include "../src/sidebar.h"
#include "../src/speedmonitor.h"
#include "../src/theme.h"
#include "../src/throttleengine.h"
#include "../src/usagetracker.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextStream>

#include <algorithm>
#include <cstdio>

using namespace net;

static int g_failures = 0;
static int g_checks = 0;
static QFile *g_log = nullptr;

static void logLine(const QString &s)
{
    std::printf("%s\n", qPrintable(s));
    std::fflush(stdout);
    if (g_log && g_log->isOpen()) {
        g_log->write(s.toUtf8());
        g_log->write("\n");
        g_log->flush();
    }
}

static void check(bool ok, const QString &what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        logLine(QStringLiteral("FAIL: ") + what);
    } else {
        logLine(QStringLiteral("  ok: ") + what);
    }
}

static QList<ScheduleCard *> allCards(QWidget *w)
{
    return w->findChildren<ScheduleCard *>();
}

static int storedRuleCount(SettingsStore &store)
{
    return store.load().rules.size();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QFile logFile(QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("schedule_results.txt")));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        g_log = &logFile;

    const QString scratch =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("schedule_scratch"));
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    theme::setMode(theme::Mode::Dark);

    // =====================================================================
    // Rule identity
    // =====================================================================
    logLine(QStringLiteral("== rule identity =="));
    {
        Settings s;
        Rule a;
        a.name = QStringLiteral("A");
        Rule b;
        b.name = QStringLiteral("B");
        s.rules << a << b;

        check(s.rules.at(0).id == 0, "a fresh rule carries no id");
        check(ensureRuleIds(s), "ensureRuleIds reports that it assigned ids");
        check(s.rules.at(0).id != 0 && s.rules.at(1).id != 0, "both rules received an id");
        check(s.rules.at(0).id != s.rules.at(1).id, "the ids are distinct");
        check(!ensureRuleIds(s), "ensureRuleIds is idempotent");

        const quint64 fresh = nextRuleId();
        check(fresh != s.rules.at(0).id && fresh != s.rules.at(1).id,
              "a newly minted id cannot collide with a loaded one");
    }

    // =====================================================================
    // Lookup by id is position-independent
    // =====================================================================
    logLine(QStringLiteral("== lookup by id =="));
    {
        Settings s;
        Rule a, b, c;
        a.name = QStringLiteral("A");
        b.name = QStringLiteral("B");
        c.name = QStringLiteral("C");
        s.rules << a << b << c;
        ensureRuleIds(s);

        const quint64 idB = s.rules.at(1).id;

        // Reverse the list. An index-based lookup would now resolve to the wrong
        // rule, which is how the old delete could remove the wrong one.
        std::reverse(s.rules.begin(), s.rules.end());
        const int idx = ruleIndexById(s, idB);
        check(idx >= 0, "the rule is still found after the list is reordered");
        check(idx >= 0 && s.rules.at(idx).name == QStringLiteral("B"),
              "the id resolves to the correct rule, not the old position");
        check(ruleIndexById(s, 0) == -1, "id 0 never matches a rule");
        check(ruleIndexById(s, 999999) == -1, "an unknown id returns -1");
    }

    // =====================================================================
    // Persistence round-trip
    // =====================================================================
    logLine(QStringLiteral("== persistence =="));
    SettingsStore store(scratch);
    {
        Settings s = store.load();
        s.rules.clear();
        Rule r;
        r.name = QStringLiteral("Night Schedule");
        r.from = QStringLiteral("22:00");
        r.to = QStringLiteral("07:00");
        r.days = 0x1F;
        r.down = 2u * 1024 * 1024;
        r.up = 512u * 1024;
        s.rules.append(r);
        ensureRuleIds(s);
        const quint64 savedId = s.rules.last().id;

        QString err;
        check(store.save(s, &err), "settings carrying an id save successfully");

        const Settings back = store.load();
        check(back.rules.size() == 1, "the rule is read back");
        check(back.rules.at(0).id == savedId,
              QStringLiteral("the id survives a round-trip (%1)").arg(savedId));
        check(back.rules.at(0).name == QStringLiteral("Night Schedule"), "the name survives");
        check(back.rules.at(0).down == 2u * 1024 * 1024, "the download limit survives");
        check(back.rules.at(0).up == 512u * 1024, "the upload limit survives");
        check(back.rules.at(0).days == 0x1F, "the weekday mask survives");
        check(back.rules.at(0).from == QStringLiteral("22:00")
                  && back.rules.at(0).to == QStringLiteral("07:00"),
              "the overnight window survives");
    }

    // =====================================================================
    // A settings file predating ids loads safely
    // =====================================================================
    logLine(QStringLiteral("== legacy settings =="));
    {
        const QString legacy = scratch + QStringLiteral("/legacy");
        QDir().mkpath(legacy);
        QFile f(legacy + QStringLiteral("/settings.json"));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(R"({"enabled":true,"rules":[{"name":"Old","from":"01:00","to":"02:00",)"
                    R"("downBps":1048576,"upBps":524288,"days":127,"enabled":true}]})");
            f.close();
        }
        SettingsStore legacyStore(legacy);
        const Settings s = legacyStore.load();
        check(s.rules.size() == 1, "the legacy rule loads");
        check(s.rules.at(0).id != 0, "the legacy rule is given an id on load");
        check(s.rules.at(0).name == QStringLiteral("Old"), "the legacy rule's data is untouched");
        check(s.rules.at(0).down == 1048576, "the legacy limit is preserved");
        check(s.rules.at(0).days == 127, "the legacy weekday mask is preserved");
    }

    // =====================================================================
    // Window integration
    // =====================================================================
    logLine(QStringLiteral("== window integration =="));
    {
        Settings s = store.load();
        s.rules.clear();
        for (int i = 1; i <= 3; ++i) {
            Rule r;
            r.name = QStringLiteral("Rule %1").arg(i);
            r.from = QStringLiteral("0%1:00").arg(i);
            r.to = QStringLiteral("0%1:30").arg(i);
            r.days = 0x7F;
            r.down = RateBps(i * 1024 * 1024);
            r.up = RateBps(i * 1024 * 256);
            s.rules.append(r);
        }
        ensureRuleIds(s);
        QString err;
        store.save(s, &err);
    }

    ThrottleEngine engine;
    SpeedMonitor monitor;
    UsageTracker usage;
    usage.setDataDirectory(scratch);

    MainWindow window(&store, &engine, &monitor, &usage);
    window.resize(1180, 780);
    window.show();
    QApplication::processEvents();
    QApplication::processEvents();

    // -- sidebar navigation -------------------------------------------------
    logLine(QStringLiteral("-- navigation --"));
    {
        auto *sidebar = window.findChild<Sidebar *>();
        check(sidebar != nullptr, "the window has a sidebar");

        if (sidebar) {
            check(sidebar->currentPage() == Sidebar::Dashboard,
                  "the dashboard is the default page");

            for (int p = 0; p < int(Sidebar::PageCount); ++p) {
                QPushButton *b = sidebar->buttonFor(Sidebar::Page(p));
                if (!b) {
                    check(false, QStringLiteral("page %1 has a nav button").arg(p));
                    continue;
                }
                b->click();
                QApplication::processEvents();
                check(sidebar->currentPage() == Sidebar::Page(p),
                      QStringLiteral("clicking nav item %1 selects that page").arg(p));
            }

            int checkedCount = 0;
            for (int p = 0; p < int(Sidebar::PageCount); ++p) {
                if (sidebar->buttonFor(Sidebar::Page(p))->isChecked())
                    ++checkedCount;
            }
            check(checkedCount == 1,
                  QStringLiteral("exactly one nav item is active (%1)").arg(checkedCount));

            sidebar->setCurrentPage(Sidebar::Dashboard);
            QApplication::processEvents();
        }
    }

    // -- cards render the stored rules --------------------------------------
    logLine(QStringLiteral("-- schedule cards --"));
    {
        const auto cards = allCards(&window);
        check(cards.size() == 6,
              QStringLiteral("every rule appears on both pages (%1 cards)").arg(cards.size()));

        int toggles = 0;
        int edits = 0;
        int deletes = 0;
        int checkboxes = 0;
        for (ScheduleCard *c : cards) {
            if (c->findChild<QPushButton *>(QStringLiteral("scheduleToggle")))
                ++toggles;
            for (QPushButton *b : c->findChildren<QPushButton *>()) {
                if (b->text() == QLatin1String("Edit"))
                    ++edits;
                if (b->text() == QLatin1String("Delete"))
                    ++deletes;
            }
            checkboxes += c->findChildren<QCheckBox *>().size();
        }

        check(toggles == cards.size(),
              QStringLiteral("every card has its own toggle (%1)").arg(toggles));
        check(edits == cards.size(), QStringLiteral("every card offers Edit (%1)").arg(edits));
        check(deletes == cards.size(),
              QStringLiteral("every card offers Delete (%1)").arg(deletes));
        check(checkboxes == 0,
              QStringLiteral("cards contain no selection checkboxes (%1)").arg(checkboxes));

        bool allHaveIds = true;
        for (ScheduleCard *c : cards) {
            if (c->ruleId() == 0)
                allHaveIds = false;
        }
        check(allHaveIds, "every card is bound to a real rule id");
    }

    // -- deletion: the reported bug ----------------------------------------
    logLine(QStringLiteral("-- deletion (the reported bug) --"));
    {
        check(storedRuleCount(store) == 3, "three rules are stored before deleting");

        const Settings before = store.load();
        const quint64 middleId = before.rules.at(1).id;
        const QString middleName = before.rules.at(1).name;
        const QString firstName = before.rules.at(0).name;
        const QString lastName = before.rules.at(2).name;
        logLine(QStringLiteral("     deleting the MIDDLE rule \"%1\"").arg(middleName));

        ScheduleCard *target = nullptr;
        for (ScheduleCard *c : allCards(&window)) {
            if (c->ruleId() == middleId) {
                target = c;
                break;
            }
        }
        check(target != nullptr, "the target card is identifiable by its id");
        check(target && target->findChildren<QPushButton *>().size() >= 2,
              "the target card carries its action buttons");

        bool removed = false;
        const bool invoked = QMetaObject::invokeMethod(
            &window, "removeSchedule", Qt::DirectConnection, Q_RETURN_ARG(bool, removed),
            Q_ARG(quint64, middleId));
        check(invoked, "the removal entry point is invokable");
        check(removed, "removeSchedule reports success");

        const Settings after = store.load();
        check(after.rules.size() == 2,
              QStringLiteral("exactly one rule was removed (%1 remain)").arg(after.rules.size()));
        check(ruleIndexById(after, middleId) == -1, "the deleted rule is gone by id");

        bool firstIntact = false;
        bool lastIntact = false;
        for (const Rule &r : after.rules) {
            if (r.name == firstName)
                firstIntact = true;
            if (r.name == lastName)
                lastIntact = true;
        }
        check(firstIntact && lastIntact,
              "the unrelated rules are untouched (a positional delete would have hit one)");

        QApplication::processEvents();
        check(allCards(&window).size() == 4,
              QStringLiteral("the cards were rebuilt after deletion (%1)")
                  .arg(allCards(&window).size()));

        check(ruleIndexById(store.load(), middleId) == -1,
              "the deleted rule does not reappear after re-reading settings");
    }

    // -- deleting a disabled rule ------------------------------------------
    logLine(QStringLiteral("-- deleting a disabled rule --"));
    {
        Settings s = store.load();
        s.rules[1].enabled = false;
        const quint64 disabledId = s.rules.at(1).id;
        QString err;
        store.save(s, &err);

        bool removed = false;
        QMetaObject::invokeMethod(&window, "removeSchedule", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, removed), Q_ARG(quint64, disabledId));
        check(removed, "a disabled rule can be deleted");
        check(ruleIndexById(store.load(), disabledId) == -1, "the disabled rule is gone");
    }

    // -- deleting the last remaining rule -----------------------------------
    logLine(QStringLiteral("-- deleting the last rule --"));
    {
        while (storedRuleCount(store) > 0) {
            const quint64 id = store.load().rules.first().id;
            bool removed = false;
            QMetaObject::invokeMethod(&window, "removeSchedule", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, removed), Q_ARG(quint64, id));
            if (!removed)
                break;
        }
        check(storedRuleCount(store) == 0, "the rule list can be emptied completely");

        QApplication::processEvents();
        check(allCards(&window).isEmpty(),
              QStringLiteral("no cards remain when no rules exist (%1)")
                  .arg(allCards(&window).size()));
        check(storedRuleCount(store) == 0,
              "no default rule is recreated after deleting them all");
    }

    // -- edit must not duplicate --------------------------------------------
    logLine(QStringLiteral("-- edit must not duplicate --"));
    {
        Rule r;
        r.name = QStringLiteral("Editable");
        r.from = QStringLiteral("09:00");
        r.to = QStringLiteral("17:00");
        r.down = 3u * 1024 * 1024;

        bool added = false;
        QMetaObject::invokeMethod(&window, "addSchedule", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, added), Q_ARG(Rule, r));
        check(added, "a rule can be added");

        const Settings afterAdd = store.load();
        check(afterAdd.rules.size() == 1, "the added rule is stored");
        const quint64 id = afterAdd.rules.first().id;

        Rule edit = afterAdd.rules.first();
        edit.name = QStringLiteral("Edited");
        edit.down = 8u * 1024 * 1024;

        bool updated = false;
        QMetaObject::invokeMethod(&window, "updateSchedule", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, updated), Q_ARG(Rule, edit));
        check(updated, "the rule can be updated");

        const Settings afterEdit = store.load();
        check(afterEdit.rules.size() == 1,
              QStringLiteral("editing does not create a duplicate (%1 rules)")
                  .arg(afterEdit.rules.size()));
        check(afterEdit.rules.first().id == id, "the id is unchanged by an edit");
        check(afterEdit.rules.first().name == QStringLiteral("Edited"), "the name was updated");
        check(afterEdit.rules.first().down == 8u * 1024 * 1024, "the limit was updated");
    }

    // -- enabling and disabling --------------------------------------------
    logLine(QStringLiteral("-- enable / disable --"));
    {
        Settings s = store.load();
        const quint64 id = s.rules.first().id;
        const bool wasEnabled = s.rules.first().enabled;

        const bool invoked = QMetaObject::invokeMethod(
            &window, "onScheduleEnabledChanged", Qt::DirectConnection, Q_ARG(quint64, id),
            Q_ARG(bool, !wasEnabled));
        check(invoked, "the enable toggle entry point is invokable");

        const Settings after = store.load();
        check(after.rules.first().enabled == !wasEnabled, "toggling persists the new state");
        check(after.rules.size() == 1, "toggling does not add or remove rules");

        Settings disabled = after;
        disabled.rules.first().enabled = false;
        disabled.enabled = true;
        const EffectiveLimits limits = evaluate(disabled, QDateTime::currentDateTime());
        check(limits.ruleIndex < 0, "a disabled rule is not applied");
    }

    // -- the Add/Edit dialog -----------------------------------------------
    logLine(QStringLiteral("-- add / edit dialog --"));
    {
        Rule blank;
        blank.name.clear();
        ScheduleDialog dlg(blank, true, nullptr);
        check(!dlg.validationError().isEmpty(), "an empty name is rejected");

        dlg.nameEdit()->setText(QStringLiteral("Good"));
        check(dlg.validationError().isEmpty(), "a valid name clears the error");

        dlg.fromEdit()->setText(QStringLiteral("99:99"));
        check(!dlg.validationError().isEmpty(), "an invalid start time is rejected");

        dlg.fromEdit()->setText(QStringLiteral("22:00"));
        dlg.toEdit()->setText(QStringLiteral("nonsense"));
        check(!dlg.validationError().isEmpty(), "an invalid end time is rejected");

        dlg.toEdit()->setText(QStringLiteral("07:00"));
        check(dlg.validationError().isEmpty(),
              "an overnight window is accepted (22:00 -> 07:00)");

        const Settings s = store.load();
        ScheduleDialog editDlg(s.rules.first(), false, nullptr);
        check(editDlg.nameEdit()->text() == s.rules.first().name,
              "edit mode is seeded with the rule's current name");
        check(editDlg.fromEdit()->text() == s.rules.first().from,
              "edit mode is seeded with the current start time");
        check(editDlg.dayPicker() != nullptr, "edit mode exposes the weekday picker");
    }

    // -- status reporting uses real state -----------------------------------
    logLine(QStringLiteral("-- status reporting --"));
    {
        auto *sidebar = window.findChild<Sidebar *>();
        if (sidebar) {
            sidebar->setCurrentPage(Sidebar::Scheduler);
            QApplication::processEvents();
        }

        QString statusText;
        for (QLabel *l : window.findChildren<QLabel *>()) {
            if (l->isVisible()
                && (l->text().startsWith(QLatin1String("Active:"))
                    || l->text().startsWith(QLatin1String("Inactive:"))
                    || l->text().startsWith(QLatin1String("Next:")))) {
                statusText = l->text();
            }
        }
        check(!statusText.isEmpty(), "the Scheduler page reports a status line");
        logLine(QStringLiteral("     status: %1").arg(statusText));
        check(!statusText.contains(QLatin1String("4h 15m")),
              "the status is not the mockup's fabricated countdown");
    }

    window.close();
    QDir(scratch).removeRecursively();

    logLine(QStringLiteral("\n%1 checks, %2 failures").arg(g_checks).arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
