# start_kenshi.ps1 - launch a Kenshi instance and get it past the launcher.
#
# Kenshi's startup shows a Win32/MFC graphics launcher (Video settings / Mods /
# Changelog, with an OK button) and blocks there until OK is clicked. RE_Kenshi
# and our plugin only load AFTER that click. The root kenshi_x64.exe is just a
# loader that relaunches the real game as a separate process named "Kenshi_x64",
# so we also have to discover that real process (a different PID).
#
# This script:
#   1. snapshots existing Kenshi_x64 PIDs,
#   2. launches the loader (default: the Steam install's kenshi_x64.exe),
#   3. waits for a NEW Kenshi_x64 window that has an "OK" button,
#   4. clicks OK via BM_CLICK (cross-process; no screen coordinates needed),
#   5. dismisses Kenshi's "Mods warning" box if the click raises one,
#   6. returns the real game's process Id (written to stdout as "GAMEPID=<id>").
#
# The click is POSTED, not sent: BM_CLICK runs the launcher's OK handler, and when
# that handler opens a modal box a SendMessage never returns - the 2026-10-21 runs
# hung for 14 minutes on exactly that, past every timeout in this script.
#
# "Mods warning" ("One or more mods are missing or in incorrect order. Continue
# anyway?") was seen once, on the join copy's FIRST launch while the host ran, on
# a Steam account subscribed to 46 Workshop mods (data\__mods.list names them all).
# It did not reappear on that copy's later launches, alone or beside the host, so
# the trigger is not pinned down. Only mods listed in data\mods.cfg load, so
# answering Yes changes nothing the test depends on; it is logged when it happens.
#
# Env vars (KENSHICOOP_*) must be set by the caller before invoking; they are
# inherited by the launched process and propagate to the relaunched game.

param(
    [string]$ExePath = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi\kenshi_x64.exe",
    [string]$WorkDir = "",
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = "Stop"
if (-not $WorkDir) { $WorkDir = Split-Path $ExePath }

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class KenLauncher {
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  public const uint BM_CLICK = 0x00F5;
  public const int  IDYES    = 6;
  // A visible top-level dialog of process pid whose title is exactly 'title'.
  public static IntPtr FindDialog(uint pid, string title) {
    IntPtr hit = IntPtr.Zero;
    EnumWindows((h,p) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if (owner != pid || !IsWindowVisible(h)) return true;
      var c = new StringBuilder(64); GetClassName(h, c, 64);
      var t = new StringBuilder(128); GetWindowText(h, t, 128);
      if (c.ToString() == "#32770" && t.ToString() == title) { hit = h; return false; }
      return true;
    }, IntPtr.Zero);
    return hit;
  }
  // Press Yes by control id, so a localized "&Evet"/"&Ja" button still matches.
  public static bool PressYes(IntPtr dlg) {
    IntPtr b = GetDlgItem(dlg, IDYES);
    if (b == IntPtr.Zero) return false;
    return PostMessage(b, BM_CLICK, IntPtr.Zero, IntPtr.Zero);
  }
  static IntPtr found;
  public static IntPtr FindButton(IntPtr top, string text) {
    found = IntPtr.Zero;
    EnumChildWindows(top, (h,p) => {
      var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() == "Button") {
        var t = new StringBuilder(64); GetWindowText(h, t, 64);
        if (t.ToString() == text) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static bool ClickButton(IntPtr top, string text) {
    IntPtr b = FindButton(top, text);
    if (b == IntPtr.Zero) return false;
    return PostMessage(b, BM_CLICK, IntPtr.Zero, IntPtr.Zero);
  }
}
"@

$before = @{}
foreach ($p in (Get-Process -Name "Kenshi_x64" -ErrorAction SilentlyContinue)) { $before[$p.Id] = $true }

Write-Host ("Launching: " + $ExePath)
Start-Process -FilePath $ExePath -WorkingDirectory $WorkDir | Out-Null

# Find a NEW Kenshi_x64 window that exposes the launcher's OK button, then click it.
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$game = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $cands = Get-Process -Name "Kenshi_x64" -ErrorAction SilentlyContinue |
             Where-Object { -not $before.ContainsKey($_.Id) -and $_.MainWindowHandle -ne 0 }
    foreach ($c in $cands) {
        if ([KenLauncher]::FindButton($c.MainWindowHandle, "OK") -ne [IntPtr]::Zero) { $game = $c; break }
    }
    if ($game) { break }
}
if (-not $game) { Write-Error "Timed out waiting for the Kenshi launcher (OK button)."; exit 2 }

Write-Host ("Launcher up: PID=" + $game.Id + " '" + $game.MainWindowTitle + "' - clicking OK")
if (-not [KenLauncher]::ClickButton($game.MainWindowHandle, "OK")) {
    Write-Error "Found launcher window but failed to click OK."; exit 3
}

# The OK handler may raise "Mods warning" (see header). Watch briefly; the game
# leaves the launcher within a second or two when no box appears.
$warnDeadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $warnDeadline) {
    Start-Sleep -Milliseconds 250
    if ($game.HasExited) { break }
    $dlg = [KenLauncher]::FindDialog([uint32]$game.Id, "Mods warning")
    if ($dlg -ne [IntPtr]::Zero) {
        if ([KenLauncher]::PressYes($dlg)) {
            Write-Host "Dismissed Kenshi 'Mods warning' (Workshop mods not visible to this instance) - continuing"
        } else {
            Write-Error "Kenshi 'Mods warning' is up and its Yes button was not found."; exit 4
        }
        break
    }
}

# Emit on the output stream (not Write-Host) so callers can capture it.
Write-Output ("GAMEPID=" + $game.Id)
exit 0
