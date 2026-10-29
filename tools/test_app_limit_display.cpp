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
    check(shown(mb, mb / 4), "leaving the schedule restores the saved base-rate display");
    rule.windows[0].enabled = true;
    engine.setRules({rule});
    check(shown(3 * mb, mb / 4), "entering the schedule updates the existing card without rebuilding it");
    engine.refresh();
    check(shown(3 * mb, mb / 4), "periodic refresh keeps the scheduled rate visible");
    check(rule.down == mb && rule.up == mb / 4, "display updates preserve saved base limits");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
