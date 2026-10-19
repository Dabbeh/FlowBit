# Flowbit - Control Your Connection.
# System-wide internet bandwidth control for Windows (Qt Widgets, MinGW).
#
# Build:
#   qmake Flowbit.pro CONFIG+=release
#   mingw32-make -j
#
# The WinDivert runtime (WinDivert.dll + WinDivert64.sys) is copied next to the
# executable automatically by the post-link step below.

QT += core gui widgets svg network

CONFIG += c++17 windows
CONFIG -= console
TEMPLATE = app
TARGET = Flowbit

# Per-configuration output dirs, mirroring the layout used by the scripts.
CONFIG(debug, debug|release) {
    DESTDIR = $$PWD/build/debug
} else {
    DESTDIR = $$PWD/build/release
}
OBJECTS_DIR = $$DESTDIR/.obj
MOC_DIR     = $$DESTDIR/.moc
RCC_DIR     = $$DESTDIR/.rcc
UI_DIR      = $$DESTDIR/.ui

INCLUDEPATH += $$PWD/src

# GetIfTable2 / MIB_IF_ROW2 / FreeMibTable (used by SpeedMonitor) live in
# netioapi.h, which iphlpapi.h only pulls in when NTDDI_VERSION is Windows 7+.
# MinGW predefines an older NTDDI_VERSION, so override it here (and _WIN32_WINNT
# with it) before any system header is parsed.
DEFINES += NTDDI_VERSION=0x06010000 _WIN32_WINNT=0x0601 WINVER=0x0601

# ---------------------------------------------------------------------------
# Source encoding: pin the compiler's input and execution charsets to UTF-8 so a
# non-ASCII glyph in a string literal is interpreted correctly regardless of the
# machine's locale. Critical arrows also use \u escapes in the C++ source, so the
# build is not solely dependent on this flag.
# ---------------------------------------------------------------------------
QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

# ---------------------------------------------------------------------------
# WinDivert 2.2.2-A (bundled under third_party/WinDivert)
# ---------------------------------------------------------------------------
WINDIVERT_ROOT = $$PWD/third_party/WinDivert/WinDivert-2.2.2-A
WINDIVERT_ARCH = $$WINDIVERT_ROOT/x64

INCLUDEPATH += $$WINDIVERT_ROOT/include
LIBS += -L$$WINDIVERT_ARCH -lwindivert
LIBS += -liphlpapi -lws2_32 -lshell32 -luser32
# ETW session management and payload decoding for per-application monitoring.
LIBS += -ladvapi32 -ltdh
# WinVerifyTrust, used by the diagnostics report to check the driver signature.
LIBS += -lwintrust -lcrypt32
LIBS += -ldwmapi

SOURCES += \
    src/main.cpp \
    src/selftest.cpp \
    src/brandmark.cpp \
    src/autostart.cpp \
    src/checkmark.cpp \
    src/crossticon.cpp \
    src/scrollbaranimator.cpp \
    src/updater.cpp \
    src/busydots.cpp \
    src/navicons.cpp \
    src/navbutton.cpp \
    src/sidebar.cpp \
    src/appmonitor.cpp \
    src/applimitengine.cpp \
    src/applimiterpage.cpp \
    src/appruledialog.cpp \
    src/diagnostics.cpp \
    src/diagnosticspanel.cpp \
    src/appnames.cpp \
    src/appactivitypanel.cpp \
    src/responsivepair.cpp \
    src/scheduledialog.cpp \
    src/schedulecard.cpp \
    src/throttleengine.cpp \
    src/schedule.cpp \
    src/settingsstore.cpp \
    src/speedmonitor.cpp \
    src/units.cpp \
    src/usagetracker.cpp \
    src/theme.cpp \
    src/speedgraph.cpp \
    src/trackslider.cpp \
    src/segmentedbar.cpp \
    src/toggleswitch.cpp \
    src/daypicker.cpp \
    src/rateeditor.cpp \
    src/rulerow.cpp \
    src/mainwindow.cpp

HEADERS += \
    src/selftest.h \
    src/throttleengine.h \
    src/settings.h \
    src/schedule.h \
    src/settingsstore.h \
    src/speedmonitor.h \
    src/units.h \
    src/usagetracker.h \
    src/theme.h \
    src/speedgraph.h \
    src/trackslider.h \
    src/segmentedbar.h \
    src/toggleswitch.h \
    src/daypicker.h \
    src/rateeditor.h \
    src/rulerow.h \
    src/brandmark.h \
    src/autostart.h \
    src/checkmark.h \
    src/crossticon.h \
    src/scrollbaranimator.h \
    src/updater.h \
    src/busydots.h \
    src/navicons.h \
    src/navbutton.h \
    src/sidebar.h \
    src/appmonitor.h \
    src/applimitengine.h \
    src/applimiterpage.h \
    src/appruledialog.h \
    src/diagnostics.h \
    src/diagnosticspanel.h \
    src/appnames.h \
    src/appactivitypanel.h \
    src/responsivepair.h \
    src/scheduledialog.h \
    src/schedulecard.h \
    src/mainwindow.h

RC_FILE = src/app.rc

# ---------------------------------------------------------------------------
# Ship the WinDivert user-mode DLL and signed driver next to the binary, plus
# the MinGW runtime DLLs so the folder is runnable on a clean machine.
# ---------------------------------------------------------------------------
win32 {
    WINDIVERT_DLL = $$shell_path($$WINDIVERT_ARCH/WinDivert.dll)
    WINDIVERT_SYS = $$shell_path($$WINDIVERT_ARCH/WinDivert64.sys)
    DEST = $$shell_path($$DESTDIR)

    QMAKE_POST_LINK += $$quote(cmd /c copy /y \"$$WINDIVERT_DLL\" \"$$DEST\" >nul $$escape_expand(\\n\\t))
    QMAKE_POST_LINK += $$quote(cmd /c copy /y \"$$WINDIVERT_SYS\" \"$$DEST\" >nul $$escape_expand(\\n\\t))

    MINGW_BIN = $$dirname(QMAKE_CXX)
    for(lib, LIBCORE) {
        QMAKE_POST_LINK += $$quote(cmd /c copy /y \"$$shell_path($$MINGW_BIN/$${lib}.dll)\" \"$$DEST\" >nul $$escape_expand(\\n\\t))
    }
}

# Extra runtime DLLs required by MinGW builds of Qt.
win32 {
    for(extra, EXTRA_DLLS) {
        QMAKE_POST_LINK += $$quote(cmd /c copy /y \"$$shell_path($$extra)\" \"$$DEST\" >nul $$escape_expand(\\n\\t))
    }
}
