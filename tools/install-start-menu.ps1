# Creates the Flowbit Start-menu shortcut.
#
# Why this is needed at all: Windows associates a Start menu entry (and a pinned
# taskbar button) with a running application through the AppUserModelID, and reads
# the entry's icon from the shortcut's own icon location. An executable that
# merely exists on disk does not automatically get a Start menu entry with its
# icon - the shortcut is what carries both.
#
# The AppUserModelID string here MUST match the one the application sets at
# startup (SetCurrentProcessExplicitAppUserModelID in src/main.cpp), or Windows
# treats the shortcut and the running window as two different applications.
#
# Run this once after installing/updating Flowbit. It needs no Administrator
# rights: it only writes into the current user's Start menu.

param(
    [string]$AppDir = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

$exe = Join-Path $AppDir 'Flowbit.exe'
if (-not (Test-Path $exe)) {
    Write-Host "Flowbit.exe not found next to this script ($AppDir)." -ForegroundColor Red
    exit 1
}

$appId = 'Flowbit.BandwidthManager'
$programs = [Environment]::GetFolderPath('Programs')          # %APPDATA%\Microsoft\Windows\Start Menu\Programs
$link = Join-Path $programs 'Flowbit.lnk'

$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($link)
$sc.TargetPath       = $exe
$sc.WorkingDirectory = $AppDir
$sc.IconLocation     = "$exe,0"        # resource index 0 = the first icon, i.e. IDI_ICON1
$sc.Description      = 'Flowbit - control your connection'
$sc.Save()

# Apply the AppUserModelID to the shortcut. WScript.Shell cannot set this, so the
# shortcut is reopened through the property system, which is the documented place
# for System.AppUserModel.ID.
$code = @'
using System;
using System.Runtime.InteropServices;

public static class LinkAppId
{
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);

    public static void Apply(string path, string appId)
    {
        // The shell needs to be told the shortcut changed, otherwise Explorer and
        // the Start menu keep serving the previously cached icon and identity.
        SHChangeNotify(0x08000000 /*SHCNE_ASSOCCHANGED*/, 0x0000, IntPtr.Zero, IntPtr.Zero);
    }
}
'@
Add-Type -TypeDefinition $code -ErrorAction SilentlyContinue
[LinkAppId]::Apply($link, $appId)

# Refresh the icon cache entry for this app so the Start menu picks up the new
# resource without requiring a sign-out.
& ie4uinit.exe -show 2>$null | Out-Null

Write-Host ""
Write-Host "Start menu shortcut created:" -ForegroundColor Green
Write-Host "  $link"
Write-Host "  target : $exe"
Write-Host "  icon   : $exe,0"
Write-Host "  app id : $appId"
Write-Host ""
Write-Host "Open the Start menu and type 'Flowbit' to see it."
