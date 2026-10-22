<#
.SYNOPSIS
  Package the PLAYER release: a folder called "KenshiCoop" with the mod files
  inside, plus a one-click installer that puts it (and RE_Kenshi) in place. The
  folder can still be copied into <Kenshi>\mods\ by hand. No bundled save.

.DESCRIPTION
  Assembles dist\mod-kit\ as:
    KenshiCoop\                 <- the drop-in mod folder (copy this into mods\)
      KenshiCoop.dll              the plugin (protocol-version-matched; a mismatch
                                  is rejected at handshake by design)
      KenshiCoop.mod              mod-list entry so it shows in Kenshi's Mods menu
      RE_Kenshi.json              tells RE_Kenshi to load the plugin
      coop_config.json            only needed for LAN/direct-UDP; Steam play is
                                  configured entirely in-game (F2)
    KenshiCoop-Setup.cmd/.ps1   <- one-click installer / uninstaller (scripts\installer)
    README.txt                  <- install + play instructions (NOT copied into
                                  mods, so it never clutters the game folder)
  ...then zips it to dist\KenshiCoop-kit.zip (the release artifact the README
  and the GitHub release point at).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1

.EXAMPLE
  # Reuse the current build instead of recompiling.
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    # Release version shown in README/PROVENANCE and in the zip name
    # (major.PROTOCOL.patch: a new protocol bumps the middle number).
    [string]$Version = "0.58.0",
    # Where to find KenshiCoop.mod / RE_Kenshi.json if they aren't in dist\mods.
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if (-not $SkipBuild) {
    # The PLAYER release ships the Release config: the shipped DLL excludes the
    # scenario harness (~12k lines) and does not define KENSHICOOP_HARNESS
    # (Phase 1 build separation). The test pipeline uses Harness instead.
    Write-Host "=== build plugin (Release / shipped, no scenario harness) ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" Release"
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

# Resolve the four mod files from the first place each exists.
function Resolve-First([string[]]$candidates, [string]$what) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    throw "$what not found (looked in: $($candidates -join '; '))"
}
$dll  = Resolve-First @(
    (Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"),
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.dll")
) "KenshiCoop.dll"

# Canonical shipped-DLL hash (Phase 1 provenance). Package from ONE DLL and
# assert the packaged copy is byte-identical to it, so the release artifact's
# SHA-256 is verifiable rather than a mutable file tracked under dist\.
$canonSha = (Get-FileHash -Algorithm SHA256 $dll).Hash
Write-Host "Canonical Release DLL SHA-256: $canonSha"
Write-Host "  source: $dll"
$json = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"),
    (Join-Path $HostDir  "mods\KenshiCoop\RE_Kenshi.json")
) "RE_Kenshi.json"
$mod  = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.mod"),
    (Join-Path $HostDir  "mods\KenshiCoop\KenshiCoop.mod")
) "KenshiCoop.mod"

# Rebuild dist\mod-kit from scratch so no stale install script survives.
$kitDir  = Join-Path $repoRoot "dist\mod-kit"
$modDir  = Join-Path $kitDir "KenshiCoop"
if (Test-Path $kitDir) { Remove-Item -Recurse -Force $kitDir }
New-Item -ItemType Directory -Force -Path $modDir | Out-Null

Write-Host "=== assembling KenshiCoop mod folder ==="
Copy-Item $dll  (Join-Path $modDir "KenshiCoop.dll")
Copy-Item $json (Join-Path $modDir "RE_Kenshi.json")
Copy-Item $mod  (Join-Path $modDir "KenshiCoop.mod")

# coop_config.json (LAN/UDP only; Steam play needs no config). Written fresh so
# the release always ships a clean default.
@'
{
  // KenshiCoop config. For a normal Steam game you do NOT need to edit this file:
  // your friend's Steam ID is entered in-game (press F2, click "Copy my Steam ID"
  // to share yours, then "Paste friend's Steam ID" to enter theirs), and nothing
  // is written back to disk.
  //
  // This file only matters for a LAN / direct-UDP game: set "transport": "udp"
  // and put the host's address in "ip" (and "port" if you changed it). ip/port are
  // re-read each time you click Connect, so you can edit them without restarting.
  "transport": "steam",
  "ip": "127.0.0.1",
  "port": 27800,
  "autoConnect": false,
  // Key that opens the co-op panel, "F1".."F12". RE_Kenshi uses F2-F4 for game
  // speed by default; if F2 also changes your speed, pick a free key here.
  "panelKey": "F2",
  // Older copies of each save kept before it is overwritten (0 = off), in
  // %LOCALAPPDATA%\kenshi\KenshiCoop_backups\<save>\<date_time>\ - copy one back
  // into your save folder to restore it.
  "saveBackups": 3,
  // HOST only: what the joining player may do. Also switchable live in the F2
  // panel while hosting. true = allowed.
  "joinCanBuild": true,
  "joinCanUseContainers": true,
  "joinCanBuy": true
}
'@ | Set-Content (Join-Path $modDir "coop_config.json") -Encoding UTF8

# One-click installer (scripts\installer): finds Kenshi, installs RE_Kenshi (and
# the 1.0.65 executable it runs on a 1.0.68 install), installs + enables the mod,
# and uninstalls. It sits at the kit root and installs the KenshiCoop folder
# next to it.
$installerDir = Join-Path $scriptDir "installer"
foreach ($f in "KenshiCoop-Setup.cmd", "KenshiCoop-Setup.ps1") {
    Copy-Item (Join-Path $installerDir $f) (Join-Path $kitDir $f)
}

# Top-level README (sibling to the KenshiCoop folder, so it is NOT copied into
# the game).
@'
KenshiCoop __VERSION__ - co-op mod
======================

INSTALL (both players)
----------------------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it anywhere.
  2. Close Kenshi, then double-click KenshiCoop-Setup.cmd.
     It finds your Kenshi, installs RE_Kenshi (the free loader the co-op plugin
     needs; downloaded from github.com/BFrizzleFoShizzle/RE_Kenshi and checked
     against a fixed SHA-256), installs the KenshiCoop mod and enables it.
     On Kenshi 1.0.68 it also creates the 1.0.65 game executable RE_Kenshi
     runs on - your own 1.0.68 executable is not modified.
  3. Start Kenshi normally. The main menu should read "RE_Kenshi 0.3.5".

  Kenshi somewhere other than a Steam library? Run it from a command prompt:
     KenshiCoop-Setup.cmd -KenshiDir "D:\Games\Kenshi"

  Manual install (if you prefer): install RE_Kenshi 0.3.1+ yourself
  (https://www.nexusmods.com/kenshi/mods/847), copy the "KenshiCoop" folder into
  <Kenshi>\mods\ and enable "KenshiCoop" in Kenshi's Mods menu.

PREREQUISITES (every player)
----------------------------
  1. Kenshi 1.0.65 or 1.0.68 on Steam.
  2. Steam RUNNING and ONLINE. No port forwarding, no IP addresses.
  3. Everyone installs the SAME KenshiCoop release (the game refuses a
     different version) and the same enabled mods.

PLAY
----
  Press F2 for the Co-op panel (main menu or in game).

  HOST - load your save or start a new game ("Multiplayer Wanderer" in
  the New Game list is a ready two-squad start), press F2 and click
  "Host for friends". Your game is now open to your Steam friends.
  "Invite a Steam friend" also sends them a Steam invite.

  JOIN - start Kenshi and stay at the MAIN MENU (no save needed). Press F2:
  under "Join a friend" every Steam friend who is hosting is listed - click
  "Join <name>'s game". The host's world is sent to you and loads by itself.

  Each player controls their own squad tab, named after their Steam name. Any
  game start works: a player without a tab gets a free one or a new character
  of their own, and gets the same tab back next time.
  The host's save is the shared save: only the host saves.
  Leave with "Leave session" (join) or "Stop hosting" (host). A player who
  leaves keeps their squad in the world as "<name> (away)": it stands still and
  takes no melee damage until they come back.

  Only the HOST pauses (Space, or "Pause game" in the panel) and picks the game
  speed; trade and dialogue windows no longer stop the game for everyone.
  Players can give items to each other but not take from another player's
  characters, unless the host turns on "Players may take from each other"
  (panel > Host settings).

  The old screens (role / transport / Steam ID copy-paste, LAN) are under
  "Advanced (IP / Steam ID)" at the bottom of the panel.

PLAY (LAN / direct UDP - advanced)
----------------------------------
  Skip the Steam ID swap. Open <Kenshi>\mods\KenshiCoop\coop_config.json in
  Notepad, set "transport": "udp", and put the host's address in "ip" (and
  "port" if you changed it). In the panel set Transport: UDP, pick Host/Join,
  and go ONLINE. ip/port are re-read whenever you go ONLINE, so no restart is
  needed after an edit.

UNINSTALL
---------
  KenshiCoop-Setup.cmd -Uninstall                  removes the mod
  KenshiCoop-Setup.cmd -Uninstall -RemoveReKenshi  also removes RE_Kenshi, if the
                                                   installer put it there
  The installer kept a backup of every file it edited (*.kenshicoop.bak).

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (Steam): both Steams must be RUNNING and ONLINE, and each side
    must have Pasted the OTHER player's ID (the panel shows the captured ID -
    confirm it matches). If "Paste friend's Steam ID" says the clipboard wasn't
    a Steam ID, have your friend re-copy theirs with "Copy my Steam ID". Look for
    '[steam] session ... active=1' in <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": one player has an older/newer build; both should use
    the same release.
'@ | Set-Content (Join-Path $kitDir "README.txt") -Encoding UTF8
$readme = Join-Path $kitDir "README.txt"
(Get-Content $readme -Raw).Replace('__VERSION__', $Version) | Set-Content $readme -Encoding UTF8

# Turkish install + test guide for players (tracked in docs/, shipped as-is).
$trGuide = Join-Path $repoRoot "docs\KURULUM_TR.txt"
if (Test-Path $trGuide) { Copy-Item $trGuide (Join-Path $kitDir "KURULUM_TR.txt") }

# Provenance: assert the PACKAGED DLL is byte-identical to the canonical build,
# then record the hash next to the kit so the release artifact is verifiable.
$packagedDll = Join-Path $modDir "KenshiCoop.dll"
$packagedSha = (Get-FileHash -Algorithm SHA256 $packagedDll).Hash
if ($packagedSha -ne $canonSha) {
    throw "packaged DLL hash ($packagedSha) != canonical Release DLL hash ($canonSha)"
}
$protoLine = Select-String -Path (Join-Path $repoRoot "src\netproto\Wire.h") `
    -Pattern 'PROTOCOL_VERSION\s*=\s*(\d+)' | Select-Object -First 1
$proto = if ($protoLine) { $protoLine.Matches[0].Groups[1].Value } else { "?" }
@{
    dllSha256       = $canonSha
    protocolVersion = $proto
    builtUtc        = (Get-Date).ToUniversalTime().ToString("o")
    config          = "Release"
    version         = $Version
} | ConvertTo-Json | Set-Content (Join-Path $kitDir "PROVENANCE.json") -Encoding UTF8
Write-Host "Packaged DLL SHA-256 verified == canonical."

# Zip: the archive contains the KenshiCoop\ folder, the installer, README.txt and
# PROVENANCE.json.
$zip = Join-Path $repoRoot "dist\KenshiCoop-kit.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $kitDir "*") -DestinationPath $zip

Write-Host ""
Write-Host "Mod folder: $modDir"
Write-Host "Kit zipped: $zip"
Get-ChildItem -Recurse $kitDir | ForEach-Object {
    Write-Host ("  " + $_.FullName.Substring($kitDir.Length + 1))
}
