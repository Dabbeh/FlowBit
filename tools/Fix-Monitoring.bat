@echo off
REM Flowbit - clears leftover ETW monitoring sessions.
REM
REM Why this exists: when Flowbit is force-closed (or crashes) while monitoring is
REM active, the ETW session it created outlives the process. A live leftover makes
REM the panel's monitoring start fail. Starting with a pid-suffixed session name
REM makes leftovers irrelevant to starting, but this script cleans them anyway.
REM Safe: it touches only sessions whose names start with Flowbit-AppMonitor.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo.
echo Stopping Flowbit monitoring sessions...
logman stop "Flowbit-AppMonitor" -ets >nul 2>&1

powershell -NoProfile -Command "logman -ets query | Select-String -SimpleMatch 'Flowbit-AppMonitor' | ForEach-Object { ($_ -split '\s+')[1] }" > "%TEMP%\flowbit_sessions.txt" 2>nul

set FOUND=0
for /f "usebackq delims=" %%S in ("%TEMP%\flowbit_sessions.txt") do (
    if not "%%S"=="" (
        set FOUND=1
        echo   stopping %%S
        logman stop "%%S" -ets >nul 2>&1
    )
)
del "%TEMP%\flowbit_sessions.txt" >nul 2>&1

if %FOUND%==0 echo   none found - already clean.
echo.
echo Done.
echo.
echo Now return to Flowbit and press Retry in the
echo "Application Network Activity" panel.
echo.
echo If it still fails: run Run-SelfTest.bat and send the report.
pause
