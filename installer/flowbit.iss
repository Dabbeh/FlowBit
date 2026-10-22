; Flowbit installer.
;
; Build with:
;   ISCC.exe installer\flowbit.iss
;
; WHAT THIS INSTALLS, AND WHY IT MATTERS
;
; Flowbit needs three things on disk that are easy to leave out and produce an
; application that starts and then fails:
;
;   1. the Qt runtime DLLs beside the executable;
;   2. the Qt PLUGIN directories - platforms/, styles/, imageformats/, iconengines/,
;      tls/, networkinformation/, generic/. Qt loads these at runtime by name, and a
;      missing platforms/ plugin is the classic "this application failed to start because
;      no Qt platform plugin could be initialized" dialog;
;   3. the WinDivert driver (WinDivert.dll and WinDivert64.sys) in the same folder as the
;      executable, because the library loads the .sys from its own directory.
;
; THE INSTALL LOCATION IS NOT COSMETIC
;
; Windows disables a kernel service whose registered driver file is replaced underneath
; it. Flowbit's driver is WinDivert64.sys, so an application that is replaced in place
; repeatedly - on the Desktop, or in a folder that gets rebuilt - can end up with a
; disabled driver service and every limit failing until it is repaired.
;
; Installing under Program Files puts the driver somewhere stable that only an installer
; writes to, which is why the diagnostics page recommends it. The application repairs a
; disabled service by itself, but not needing the repair is better.
;
; PRIVILEGES
;
; The application manifest requests elevation, so no installer-level privilege is needed
; for Flowbit to shape traffic - it will ask on each run. Installing to Program Files does
; need administrative rights, which is what this installer requests.

#define AppName "Flowbit"
#define AppVersion "1.0.1"
#define AppPublisher "Flowbit"
#define AppExeName "Flowbit.exe"

[Setup]
AppId={{8E4C1F62-3B7A-4D2E-9C15-6A0B7F3D8E91}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
; A stable location rather than the Desktop, for the driver reason above.
DisableDirPage=no
; 64-bit only: the driver is WinDivert64.sys and the Qt build is mingw_64.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The application is self-contained; nothing to register, nothing to restart for.
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\{#AppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Ask for elevation up front, so the Program Files write cannot fail halfway.
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename=Flowbit-{#AppVersion}-Setup
SetupIconFile=..\src\flowbit.ico
; The application keeps settings in the user profile, not here, so uninstalling must not
; remove them: a user reinstalling would otherwise lose their limits and usage history.

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Off by default. An application that adds itself to startup without being asked is the
; behaviour most people call annoying, and the settings page offers it properly. This is
; only here so an unattended install can request it.
Name: "startup"; Description: "Start Flowbit when Windows starts (minimised to the tray)"; \
    GroupDescription: "Startup:"; Flags: unchecked
Name: "desktopicon"; Description: "Create a desktop shortcut"; \
    GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
; The executable.
Source: "..\build\release\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; The Qt runtime. Every DLL the build links against must be present, and all of these
; are: core, gui, widgets, svg and network.
Source: "..\build\release\Qt6Core.dll";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\Qt6Gui.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\Qt6Widgets.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\Qt6Svg.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\Qt6Network.dll"; DestDir: "{app}"; Flags: ignoreversion

; The MinGW runtime the Qt build depends on. Omitting any of these produces an error about
; a missing libstdc++ or libgcc DLL rather than anything that names Qt.
Source: "..\build\release\libgcc_s_seh-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\libstdc++-6.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion

; Qt plugins. These are the directories Qt searches at runtime; the platforms/ one is
; mandatory or the application cannot create a window at all.
Source: "..\build\release\platforms\*";         DestDir: "{app}\platforms";         Flags: ignoreversion recursesubdirs
Source: "..\build\release\styles\*";            DestDir: "{app}\styles";            Flags: ignoreversion recursesubdirs
Source: "..\build\release\imageformats\*";      DestDir: "{app}\imageformats";      Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "..\build\release\iconengines\*";       DestDir: "{app}\iconengines";       Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "..\build\release\tls\*";               DestDir: "{app}\tls";               Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "..\build\release\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "..\build\release\generic\*";           DestDir: "{app}\generic";           Flags: ignoreversion recursesubdirs skipifsourcedoesntexist

; The driver. Both files must sit beside the executable: WinDivert.dll loads
; WinDivert64.sys from the same directory, and it registers that path with the service.
Source: "..\build\release\WinDivert.dll";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\WinDivert64.sys"; DestDir: "{app}"; Flags: ignoreversion

; The script the uninstaller runs to stop and deregister the driver in the right order. Kept
; beside the application rather than generated at uninstall time, so it is the same file every
; time and can be read.
Source: "flowbit-remove-driver.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "flowbit-stop-driver.ps1"; DestDir: "{app}"; Flags: ignoreversion

; The self-update helper the in-app updater writes. Shipped so an update can be applied
; without anything being created at run time, and so a locked-down folder still works.
; NOT included: the development-only .bat files, the test guide, or any test binary.

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
; Register the driver service.
;
; The driver cannot be registered without administrator rights, and setup already has them.
; Deferring it to first use does not work: Flowbit only opens the driver when a limit is set, and
; a fresh install has limiting off, so the service is never created and every limit fails until
; the user happens to run Flowbit elevated by hand.
;
; --register-driver opens the driver with a filter that matches nothing, which makes WinDivert
; register its own service, and closes it again. Nothing is left running.
; shellexec, not the default CreateProcess.
;
; Flowbit.exe requires administrator rights in its manifest, so Windows refuses to start it from
; a process that is not elevated - even though setup itself is elevated, Inno's default launch
; goes through CreateProcess, which cannot elevate and fails with code 740. That was the cause
; of the earlier "Unable to execute file" error after installing.
;
; runhidden cannot be combined with shellexec: ShellExecute has no way to hide a window. The wait
; is kept, because the installer must not report success before registration has finished - the
; exit code is what says whether it worked.
Filename: "{app}\{#AppExeName}"; Parameters: "--register-driver"; \
    StatusMsg: "Registering the network driver..."; \
    Flags: shellexec waituntilterminated

; Offered after install, not forced: Flowbit raises its own consent prompt when it runs,
; and starting it here means two prompts in a row.
; shellexec, not the default CreateProcess.
;
; Flowbit's embedded manifest requires administrator rights, and Inno's default launch uses
; CreateProcess, which cannot elevate an already-running unelevated parent. The install ends
; with:
;
;     Unable to execute file: C:\Program Files\Flowbit\Flowbit.exe
;     CreateProcess failed; code 740. The requested operation requires elevation.
;
; shellexec goes through ShellExecute, which reads the manifest and raises the ordinary
; consent prompt - which is the same prompt Flowbit raises when started from the Start menu,
; so nothing new is asked of the user.
;
; runasoriginaluser would be wrong here: it would launch unelevated, and an unelevated
; Flowbit cannot load its driver, so the first thing the user saw after installing would be
; a message saying limits cannot be applied.
Filename: "{app}\{#AppExeName}"; Description: "Start {#AppName} now"; \
    Flags: nowait postinstall skipifsilent shellexec

[Registry]
; The startup entry, only when the task was selected. HKCU rather than HKLM because the
; application's own setting writes there - two entries in different hives would fight, and
; the user's own toggle would appear not to work.
; Written to HKCU, and Inno redirects it to the original user during an elevated
; install. Do not move this to HKLM: the application's own "Start with Windows"
; setting writes HKCU, so an HKLM entry here would fight it and the user's toggle
; would appear not to work.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "Flowbit"; \
    ValueData: """{app}\{#AppExeName}"" --tray-start"; \
    Flags: uninsdeletevalue; Tasks: startup

[UninstallRun]
; Stop and deregister the WinDivert service.
;
; The .sys file is deleted with the folder, but the SERVICE REGISTRATION stays in Windows and
; keeps pointing at a file that is no longer there. That is the state reported as "pending
; removal", and it survives reinstalling - the next install finds the stale entry instead of
; registering its own.
;
; The stop is attempted first so the driver's file handle is released; a service whose driver
; is loaded cannot be deleted. Both are allowed to fail: another application may legitimately
; own a WinDivert service of its own, and refusing to fail here would block the uninstall.
; Stop then delete, in one script, because the order and the timing both matter.
;
; sc delete marks a service for removal; it does not remove it while the driver is still loaded.
; Issuing it immediately after sc stop therefore leaves the service pending removal until the
; next boot - and a reinstall then cannot create its own, because the name is held by one that is
; halfway gone. That is the state reported as "pending removal, which only a restart of Windows
; clears".
;
; The script waits for the service to report STOPPED before deleting it, and skips the deletion
; altogether if it never stops. Leaving the service alone is recoverable; leaving it marked for
; deletion is not, without a restart.
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; \
    Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\flowbit-remove-driver.ps1"""; \
    Flags: runhidden waituntilterminated; RunOnceId: "removedriver"

; Remove the startup task.
;
; Flowbit's own "Launch Flowbit when Windows starts" writes a scheduled task rather than a Run
; key entry, so removing the Run key alone leaves a task that tries to launch a program which
; no longer exists, on every logon.
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN Flowbit /F"; \
    Flags: runhidden waituntilterminated; RunOnceId: "deltask"

[UninstallDelete]
; The staged update file and the self-update script, if either was left behind.
Type: files; Name: "{app}\Flowbit-update.exe"
Type: files; Name: "{app}\flowbit-self-update.ps1"

; The generated checkbox tick. Recreated on the next launch, so removing it costs nothing.
Type: files; Name: "{userappdata}\..\Local\Flowbit\checkmark.svg"

[Code]
// Runs the script that stops the driver service and waits for it to unload.
//
// Returns the script's exit code: 0 when the driver is no longer loaded, 1 when it could not be
// unloaded within its deadline. The script ships beside the application rather than being
// generated here, so it is one file that can be read and tested on its own.
function RunDriverStopScript(): Integer;
var
  Script: String;
begin
  Result := 0;
  Script := ExpandConstant('{app}\flowbit-stop-driver.ps1');

  if not FileExists(Script) then
    exit;   // a first install has nothing to stop

  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
       '-NoProfile -ExecutionPolicy Bypass -File "' + Script + '"',
       '', SW_HIDE, ewWaitUntilTerminated, Result);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Result := '';

  // Close a running Flowbit before copying over it.
  //
  // Windows refuses to replace the image of a running program, and reports the refusal as
  // "DeleteFile failed; code 5. Access is denied." - which reads like a permissions problem and
  // is not one. Flowbit lives in the tray, so closing its window leaves it running, and someone
  // reinstalling has no reason to think it is still there.
  //
  // taskkill is run unconditionally, with no test first. There is no reliable way for this
  // script to ask whether a process is running: the engine has no file I/O and no process list,
  // and parsing tasklist's printed output would be more fragile than the thing it saves. A
  // taskkill that matches nothing does nothing, which is exactly the desired outcome when
  // Flowbit is already closed.
  //
  // No /F. A plain close lets Flowbit flush its usage counters on the way out; killing it would
  // lose up to 30 seconds of measured data.
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM {#AppExeName}', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);

  // Give it a moment to finish flushing and exit. taskkill returns when the signal is delivered,
  // not when the process is gone.
  Sleep(2000);

  // Unload the driver, which closing Flowbit does NOT do.
  //
  // The handle belongs to the process; the driver belongs to the service. Closing Flowbit
  // releases one and leaves the other loaded - and setup is about to write WinDivert64.sys over
  // it. Windows does not permit that, and its response is to disable the service, which leaves a
  // driver that cannot be unloaded and a service that can never load again. The application then
  // reports it on every launch and only a restart clears it.
  //
  // Stopping the service here means the file is copied while nothing holds it, which is the
  // whole difference between an update that works and one that leaves the machine stuck.
  //
  // A failure is reported and setup continues. Refusing to install because a driver would not
  // unload would be worse: the install would be impossible to complete, and the application's own
  // repair at the next launch handles it from there.
  if FileExists(ExpandConstant('{app}\WinDivert64.sys')) then
  begin
    ResultCode := RunDriverStopScript();
    if ResultCode <> 0 then
    begin
      // Only a warning. The user is told because a limit may not apply until the driver is free,
      // and because the alternative - silence - would make a later fault look unexplained.
      MsgBox('The network driver could not be unloaded, because another program is using it.'
             + #13#10 + #13#10 +
             'Setup will continue. If limits do not work afterwards, close any other Flowbit '
             + 'windows and start Flowbit again.',
             mbInformation, MB_OK);
    end;
  end;
end;



// Asks whether to delete the settings and usage history as well.
//
// They are not removed by default because they hold the limits, schedules and the 30-day usage
// figures, and the figures are measured data that cannot be recovered. A silent deletion would
// lose them; a silent keep leaves folders behind for someone who wanted a clean removal. So the
// question is asked, and the default answer is to keep them.
function InitializeUninstall(): Boolean;
var
  Answer: Integer;
  DataDir: String;
begin
  Result := True;

  DataDir := ExpandConstant('{localappdata}\Flowbit');

  if DirExists(DataDir) then
  begin
    Answer := MsgBox('Also delete your Flowbit settings and usage history?' + #13#10 + #13#10 +
                     'This removes your limits, schedules, application rules and the 30-day' + #13#10 +
                     'data usage figures.' + #13#10 + #13#10 +
                     'The usage figures are measured data and cannot be recovered. Choose No' + #13#10 +
                     'to keep everything, so a future install picks up where you left off.',
                     mbConfirmation, MB_YESNO or MB_DEFBUTTON2);

    if Answer = IDYES then
    begin
      // remove the active directory, and the older nested one if a previous version left it
      DelTree(DataDir, True, True, True);
      DelTree(ExpandConstant('{localappdata}\Flowbit\Flowbit'), True, True, True);
    end;
  end;
end;
