cmake_minimum_required(VERSION 3.19)

project(Flowbit
    VERSION 1.0.3
    DESCRIPTION "System-wide and per-application bandwidth limiter with schedules, live monitoring and diagnostics"
    LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# ---------------------------------------------------------------------------
# Source encoding
#
# The UI draws arrow glyphs in labels. Their bytes must be interpreted as UTF-8
# by the compiler or they render as mojibake ("â†“" instead of a downward arrow).
# MSVC needs an explicit flag; GCC/Clang default to UTF-8 but get it stated so a
# different locale cannot change the result. The sources also avoid relying on
# this for critical glyphs by using \u escapes in C++ string literals.
# ---------------------------------------------------------------------------
add_compile_options(
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-finput-charset=UTF-8>"
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fexec-charset=UTF-8>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

# ---------------------------------------------------------------------------
# WinDivert (bundled, see third_party/WinDivert)
# ---------------------------------------------------------------------------
set(WINDIVERT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WinDivert/WinDivert-2.2.2-A"
    CACHE PATH "WinDivert distribution root")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WINDIVERT_ARCH_DIR "${WINDIVERT_ROOT}/x64")
else()
    set(WINDIVERT_ARCH_DIR "${WINDIVERT_ROOT}/x86")
endif()

if(NOT EXISTS "${WINDIVERT_ROOT}/include/windivert.h")
    message(FATAL_ERROR "WinDivert headers not found under ${WINDIVERT_ROOT}. "
                        "Download WinDivert 2.2.2-A and extract it into third_party/WinDivert.")
endif()

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
add_executable(Flowbit WIN32
    src/main.cpp
    src/brandmark.cpp
    src/brandmark.h
    src/selftest.cpp
    src/selftest.h
    src/autostart.cpp
    src/autostart.h
    src/checkmark.cpp
    src/checkmark.h
    src/crossticon.cpp
    src/crossticon.h
    src/scrollbaranimator.cpp
    src/scrollbaranimator.h
    src/updater.cpp
    src/updater.h
    src/updatedownloaddialog.h
    src/busydots.cpp
    src/busydots.h
    src/navicons.cpp
    src/navicons.h
    src/navbutton.cpp
    src/navbutton.h
    src/theme.cpp
    src/theme.h
    src/throttleengine.cpp
    src/throttleengine.h
    src/driverprobe.h
    src/settings.h
    src/schedule.h
    src/schedule.cpp
    src/settingsstore.h
    src/settingsstore.cpp
    src/speedmonitor.h
    src/speedmonitor.cpp
    src/units.h
    src/units.cpp
    src/usagetracker.h
    src/usagetracker.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/sidebar.h
    src/sidebar.cpp
    src/speedgraph.h
    src/speedgraph.cpp
    src/trackslider.h
    src/trackslider.cpp
    src/segmentedbar.h
    src/segmentedbar.cpp
    src/toggleswitch.h
    src/toggleswitch.cpp
    src/daypicker.h
    src/daypicker.cpp
    src/rateeditor.h
    src/rateeditor.cpp
    src/rulerow.h
    src/rulerow.cpp
    src/schedulecard.h
    src/schedulecard.cpp
    src/scheduledialog.h
    src/scheduledialog.cpp
    src/responsivepair.h
    src/responsivepair.cpp
    src/appmonitor.h
    src/appmonitor.cpp
    src/etwclass.h
    src/etwclass.cpp
    src/appnames.h
    src/appnames.cpp
    src/appactivitypanel.h
    src/appactivitypanel.cpp
    src/applimitengine.h
    src/applimitengine.cpp
    src/applimiterpage.h
    src/applimiterpage.cpp
    src/appruledialog.h
    src/appruledialog.cpp
    src/diagnostics.h
    src/diagnostics.cpp
    src/diagnosticspanel.h
    src/diagnosticspanel.cpp
    src/theme.h
    src/theme.cpp
    src/checkmark.h
    src/checkmark.cpp
    src/busydots.h
    src/busydots.cpp
    src/crossticon.h
    src/crossticon.cpp
    src/navicons.h
    src/navicons.cpp
    src/navbutton.h
    src/navbutton.cpp
    src/scrollbaranimator.h
    src/scrollbaranimator.cpp
    src/autostart.h
    src/autostart.cpp
    src/etwclass.h
    src/etwclass.cpp
    src/selftest.h
    src/selftest.cpp
    src/updater.h
    src/updater.cpp
    src/app.rc
)

target_include_directories(Flowbit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WINDIVERT_ROOT}/include")

target_link_directories(Flowbit PRIVATE "${WINDIVERT_ARCH_DIR}")

target_link_libraries(Flowbit PRIVATE
    Qt6::Widgets
    Qt6::Svg
    Qt6::Network
    windivert        # WinDivert import library
    dwmapi
    iphlpapi         # GetIfTable2 for live throughput
    ws2_32
    advapi32         # ETW session control (per-application monitoring)
    tdh              # ETW payload decode by field name
    wintrust         # driver signature check (diagnostics)
    crypt32
    psapi
    shell32
    user32
    winmm            # precise Windows timing during reveal animations
)

# windres needs to find the .manifest referenced from app.rc
if(MINGW)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --include-dir \"${CMAKE_CURRENT_SOURCE_DIR}/src\"")
endif()

# Ship the WinDivert user-mode DLL and the signed driver next to the exe.
add_custom_command(TARGET Flowbit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WINDIVERT_ARCH_DIR}/WinDivert.dll"
            "$<TARGET_FILE_DIR:Flowbit>/WinDivert.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WINDIVERT_ARCH_DIR}/WinDivert64.sys"
            "$<TARGET_FILE_DIR:Flowbit>/WinDivert64.sys"
    COMMENT "Copying WinDivert runtime next to Flowbit.exe")

# ---------------------------------------------------------------------------
# Convenience: `cmake --build . --target deploy` copies Qt runtime DLLs.
# ---------------------------------------------------------------------------
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin")
if(WINDEPLOYQT_EXECUTABLE)
    add_custom_target(deploy
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations --no-system-d3d-compiler
                --no-opengl-sw "$<TARGET_FILE:Flowbit>"
        DEPENDS Flowbit
        COMMENT "Deploying Qt runtime next to Flowbit.exe")
endif()
