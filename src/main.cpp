#include "mainwindow.h"
#include "appmonitor.h"
#include "selftest.h"
#include "settingsstore.h"
#include "speedmonitor.h"
#include "theme.h"
#include "throttleengine.h"
#include "usagetracker.h"

#include <QApplication>

#include <cstdio>
#include <QDebug>
#include <QIcon>
#include <QMessageBox>
#include <QSharedMemory>
#include <QSystemTrayIcon>

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>   // SetCurrentProcessExplicitAppUserModelID

namespace {

bool isElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// Relaunch through the UAC prompt. Returns false when the user declines.
bool relaunchElevated()
{
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
        return false;

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = path;
    info.lpParameters = L"--elevated";
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info))
        return false;

    if (info.hProcess)
        CloseHandle(info.hProcess);
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    // Elevation is requested by the embedded manifest, so this normally happens
    // before main(). It still catches launches that bypass the manifest (e.g.
    // some launchers) so the failure mode is a clear message, not a silent
    // "WinDivertOpen failed with error 5".
    const bool elevatedFlag = QCoreApplication::arguments().contains(QStringLiteral("--elevated"));

    if (!isElevated()) {
        QApplication probe(argc, argv);
        QApplication::setApplicationName(QStringLiteral("Flowbit"));

        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            const auto choice = QMessageBox::question(
                nullptr, QStringLiteral("Flowbit needs Administrator rights"),
                QStringLiteral("Flowbit must run as Administrator to load the WinDivert "
                               "driver that shapes system-wide traffic.\n\n"
                               "Restart it elevated now?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (choice == QMessageBox::Yes && relaunchElevated())
                return 0;
        } else {
            QMessageBox::critical(nullptr, QStringLiteral("Flowbit"),
                                  QStringLiteral("Flowbit must be run as Administrator."));
        }
        return 1;
    }

    Q_UNUSED(elevatedFlag)

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Flowbit"));
    // The organisation name is deliberately NOT set.
    //
    // Qt joins it with the application name to build the config path. With both set to
    // "Flowbit" every file lived in Local/Flowbit/Flowbit/ - correct for Qt, but a level deeper
    // than it needs to be, and not what the path suggests to anyone reading it.
    //
    // It is also unnecessary: the organisation exists so Qt can locate shared registry keys and
    // settings for a suite of products. There is one product, and nothing here reads the
    // organisation. Existing data is migrated by SettingsStore, which recognises the nested
    // directory as a legacy location.
    QApplication::setApplicationDisplayName(QStringLiteral("Flowbit"));
    QApplication::setApplicationVersion(QStringLiteral("1.0.4"));

    // Give Windows an explicit application identity.
    //
    // Per Microsoft's AppUserModelID documentation, the shortcut, the pinned
    // taskbar button and the Start menu entry are all associated with the running
    // process through this id, and it must be set during startup BEFORE any UI is
    // shown. Without it Windows falls back to heuristics, which is how an app ends
    // up with a generic or missing icon in the Start menu even though the icon is
    // embedded in the executable - the exact symptom reported here.
    //
    // Format: CompanyName.ProductName[.SubProduct][.Version], no spaces,
    // 128 characters maximum.
    SetCurrentProcessExplicitAppUserModelID(L"Flowbit.BandwidthManager");

    // Internal helper mode: re-executed by AppMonitor (always from an already
    // elevated parent) to clear a leftover trace session from a killed run.
    // A ghost session is invisible to logman but stoppable by ControlTrace from
    // an elevated context; running it in a SEPARATE process means a stuck kernel
    // lock can never freeze the main window's UI - the parent simply bounds the
    // wait and reports if the child does not finish.
    if (app.arguments().contains(QStringLiteral("--clear-monitor-session"))) {
        net::AppMonitor::clearGhostSession();
        return 0;
    }

    // Driver registration: Flowbit.exe --register-driver
    //
    // Run by the installer, which is already elevated - the driver cannot be registered without
    // administrator rights, and requiring the user to launch Flowbit by hand from the right
    // folder is not a reasonable thing to ask.
    //
    // Opening the driver makes WinDivert register its own service; closing it immediately leaves
    // nothing running. The filter matches nothing, so no packet is touched.
    //
    // Handled before the single-instance guard, because the installer may run while the
    // application is already open - a repair install, for instance.
    if (app.arguments().contains(QStringLiteral("--register-driver"))) {
        const bool ok = net::ThrottleEngine::registerDriverService();
        // The exit code is what the installer reads, so it must be meaningful: zero only when
        // the service really is usable afterwards.
        std::fprintf(ok ? stdout : stderr,
                     "Flowbit: driver service %s\n",
                     ok ? "registered" : "could NOT be registered");
        return ok ? 0 : 1;
    }

    // Self-test mode: Flowbit.exe --selftest [report.txt]
    //
    // Deliberately handled BEFORE the single-instance guard below, so the owner can
    // check the installation while the app itself is running - which is exactly
    // when a fault is most likely to be noticed. It prints to stdout and, when a
    // path is given, writes the same report to a file; no window is created.
    if (app.arguments().contains(QStringLiteral("--selftest"))) {
        QString report;
        const QStringList args = app.arguments();
        const int at = args.indexOf(QStringLiteral("--selftest"));
        if (at >= 0 && at + 1 < args.size() && !args.at(at + 1).startsWith(QLatin1Char('-')))
            report = args.at(at + 1);
        return net::runSelfTest(report);
    }

    // The icon is embedded as a Windows resource (see src/app.rc), which is what
    // the taskbar, tray, Alt-Tab and Explorer read. There is no .qrc in this
    // project, so there is no ":/..." path to set here - Qt picks the window
    // icon up from the executable's own resources.

    // Keep the app alive when the window is closed: it lives in the tray so a
    // scheduled window can still kick in later.
    QApplication::setQuitOnLastWindowClosed(false);

    QSharedMemory guard(QStringLiteral("Flowbit-SingleInstance-v1"));
    if (!guard.create(1)) {
        QMessageBox::information(nullptr, QStringLiteral("Flowbit"),
                                 QStringLiteral("Flowbit is already running (check the "
                                                "notification area)."));
        return 0;
    }

    net::SettingsStore store;

    // Rebranding NetValve -> Flowbit moved the config directory. Carry any
    // existing settings and monthly usage history across before anything reads
    // them, so an upgrade never looks like a wiped install. The originals are
    // left in place.
    const int migrated = store.migrateLegacyData();
    if (migrated > 0) {
        qInfo("Flowbit: migrated %d file(s) from the previous NetValve data directory",
              migrated);
    }

    net::ThrottleEngine engine;
    net::SpeedMonitor monitor;
    net::UsageTracker usage;
    usage.setDataDirectory(store.directory());

    // Resolve the saved appearance preference (default: follow Windows) and
    // install the matching stylesheet before the window is built.
    const net::Settings startupSettings = store.load();
    switch (startupSettings.themeMode) {
    case net::Settings::ThemeMode::Light:
        net::theme::setMode(net::theme::Mode::Light);
        break;
    case net::Settings::ThemeMode::Dark:
        net::theme::setMode(net::theme::Mode::Dark);
        break;
    case net::Settings::ThemeMode::Auto:
        net::theme::setMode(net::theme::Mode::Auto);
        break;
    }

    net::MainWindow window(&store, &engine, &monitor, &usage);

    // Launched by Windows rather than by the user.
    //
    // The Run key entry passes --tray-start so a logon does not open a window on top of
    // whatever the user is doing. Flowbit is running and shaping traffic either way; the
    // notification area is how it is reached.
    //
    // Asking the window instead of deciding here keeps the tray icon, the tooltip and the
    // menu consistent: they are all set up by the window, and a window that was never
    // shown would otherwise have no way to tell the user it is there.
    const bool startedByWindows =
        app.arguments().contains(QStringLiteral("--tray-start"));
    if (startedByWindows)
        window.startHidden();
    else
        window.show();

    // Follow the OS while the app is running, but only in Auto mode.
    //
    // Qt6 deprecates QGuiApplication::paletteChanged in favour of handling
    // QEvent::ApplicationPaletteChange, which is what Windows raises when the
    // user flips the apps-light/dark setting. A small filter on the application
    // object observes that without the deprecated signal.
    class ColourSchemeWatcher : public QObject
    {
    public:
        ColourSchemeWatcher(net::SettingsStore *store, QWidget *window, QObject *parent)
            : QObject(parent), m_store(store), m_window(window)
        {
        }

    protected:
        bool eventFilter(QObject *obj, QEvent *event) override
        {
            if (event->type() == QEvent::ApplicationPaletteChange) {
                const net::Settings s = m_store->load();
                if (s.themeMode == net::Settings::ThemeMode::Auto) {
                    const bool wantDark = !net::theme::systemPrefersLight();
                    if (wantDark != net::theme::isDark()) {
                        net::theme::setMode(net::theme::Mode::Auto);
                        if (m_window)
                            m_window->update();
                    }
                }
            }
            return QObject::eventFilter(obj, event);
        }

    private:
        net::SettingsStore *m_store = nullptr;
        QWidget *m_window = nullptr;
    };

    auto *watcher = new ColourSchemeWatcher(&store, &window, &app);
    app.installEventFilter(watcher);

    const int rc = app.exec();

    // Persist whatever usage was accumulated in this session before exiting.
    usage.flush();
    return rc;
}
