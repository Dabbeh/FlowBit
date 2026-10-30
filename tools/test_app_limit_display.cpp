#include "applimiterpage.h"
#include "applimitengine.h"
#include "theme.h"
#include <QApplication>
#include <QDateTime>
#include <QLabel>
#include <cstdio>

using namespace net;

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const char *message) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
        failures += !ok;
    };
    constexpr RateBps mb = 1024 * 1024;
    AppRule rule;
    rule.id = 42;
    rule.exe = QStringLiteral("test.exe");
    rule.down = mb;
    rule.up = mb / 4;
    Rule window;
    window.enabled = true;
    window.days = 0x7f;
    const auto now = QDateTime::currentDateTime();
    window.from = now.addSecs(-120).time().toString(QStringLiteral("HH:mm"));
    window.to = now.addSecs(120).time().toString(QStringLiteral("HH:mm"));
    window.down = 2 * mb;
    window.up = mb / 2;
    rule.windows.append(window);
    AppLimitEngine engine; // No driver handles are opened by setRules/refresh.
    AppLimiterPage page(&engine);
    page.setRules({rule});
    auto values = [&] {
        auto *card = page.findChild<AppLimitCard *>();
        return card->findChildren<QLabel *>(QStringLiteral("limitValue"));
    };
    auto shown = [&](RateBps down, RateBps up) {
        const auto labels = values();
        return labels.size() == 2
            && labels[0]->text() == theme::formatBytesPerSec(double(down))
            && labels[1]->text() == theme::formatBytesPerSec(double(up));
    };
    engine.setRules({rule});
    check(shown(2 * mb, mb / 2), "engine notification replaces base 1 MB/s with scheduled 2 MB/s");
    check(!engine.isRunning(), "display test does not enable traffic enforcement");
    rule.windows[0].down = 3 * mb;
    engine.setRules({rule});
    check(shown(3 * mb, mb / 2), "editing a running schedule refreshes both displayed rates");
    rule.windows[0].up = kUnlimited;
    engine.setRules({rule});
    check(shown(3 * mb, mb / 4), "unspecified schedule upload inherits the saved upload rate");
    rule.windows[0].enabled = false;
    engine.setRules({rule});
    page.setRules({rule});
    check(shown(3 * mb, mb / 4), "a saved schedule retains its configured rates when inactive");
    rule.windows[0].enabled = true;
    engine.setRules({rule});
    check(shown(3 * mb, mb / 4), "entering the schedule updates the existing card without rebuilding it");
    engine.refresh();
    check(shown(3 * mb, mb / 4), "periodic refresh keeps the scheduled rate visible");
    check(rule.down == mb && rule.up == mb / 4, "display updates preserve saved base limits");
    rule.windows.clear();
    engine.setRules({rule});
    page.setRules({rule});
    check(shown(mb, mb / 4), "removing all schedules restores the original limits");

    // Exact screenshot case: the app has a 256 KB/s base rate, but the saved
    // 07:00-09:00 schedule says 384 KB/s. At 15:02 the configured card must still
    // agree with the Schedule dialog, while its status says Outside its schedule.
    rule.down = 256 * 1024;
    rule.up = mb;
    window.from = QStringLiteral("07:00");
    window.to = QStringLiteral("09:00");
    window.down = 384 * 1024;
    window.up = mb;
    rule.windows = {window};
    const QDateTime afternoon(QDate(2026, 10, 30), QTime(15, 2));
    auto state = AppLimitEngine::evaluate(rule, afternoon);
    AppLimitCard card(rule, state, true);
    auto cardShows = [&](RateBps down) {
        const auto labels = card.findChildren<QLabel *>(QStringLiteral("limitValue"));
        return labels.size() == 2 && labels[0]->text() == theme::formatBytesPerSec(double(down))
            && labels[1]->text() == theme::formatBytesPerSec(double(mb));
    };
    auto hasText = [&](const QString &part) {
        for (auto *label : card.findChildren<QLabel *>())
            if (label->text().contains(part)) return true;
        return false;
    };
    check(cardShows(384 * 1024), "07:00-09:00 schedule shows 384 KB/s at 15:02, matching its dialog");
    check(hasText(QStringLiteral("Outside its schedule")) && !state.active,
          "schedule preview does not falsely report enforcement outside its hours");
    Rule second = window;
    second.from = QStringLiteral("14:00");
    second.to = QStringLiteral("16:00");
    second.down = 512 * 1024;
    rule.windows.append(second);
    state = AppLimitEngine::evaluate(rule, afternoon);
    card.setRule(rule, state, true);
    check(cardShows(512 * 1024) && hasText(QStringLiteral("14:00")),
          "an active second schedule shows its own rates and hours together");
    rule.windows[0].enabled = false;
    state = AppLimitEngine::evaluate(rule, QDateTime(afternoon.date(), QTime(20, 0)));
    card.setRule(rule, state, true);
    check(cardShows(512 * 1024) && hasText(QStringLiteral("14:00")),
          "outside all hours the first enabled schedule is previewed consistently");
    rule.windows.clear();
    state = AppLimitEngine::evaluate(rule, afternoon);
    card.setRule(rule, state, true);
    check(cardShows(256 * 1024) && hasText(QStringLiteral("Always on")),
          "with no schedule the screenshot case shows its original 256 KB/s limit");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
