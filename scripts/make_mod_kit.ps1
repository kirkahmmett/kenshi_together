<#
.SYNOPSIS
  Package the PLAYER release of Kenshi Together: the zip kit (mod folder + a
  one-click installer) and the Steam Workshop content folder.

.DESCRIPTION
  Assembles dist\mod-kit\ as:
    KenshiTogether\                 <- the drop-in mod folder (copy this into mods\)
      KenshiTogether.dll              the plugin (protocol-version-matched; a mismatch
                                      is rejected at handshake by design)
      KenshiTogether.mod              mod-list entry (+ the two co-op game starts)
      RE_Kenshi.json                  tells RE_Kenshi to load the plugin
      coop_config.json                only needed for LAN/direct-UDP
      version.txt                     release version for the version check
    KenshiTogether-Setup.cmd/.ps1   <- one-click installer / uninstaller (scripts\installer)
    README.txt, KURULUM_TR.txt      <- install + play instructions (not copied into mods)
    PROVENANCE.json                 <- SHA-256 of the shipped DLL
  ...zips it to dist\KenshiTogether-v<version>.zip, and copies the mod folder to
  dist\workshop\KenshiTogether\ - exactly what a Workshop upload ships, with
  _KenshiTogether.img (dist\workshop\preview.png) and _KenshiTogether.info
  (dist\workshop\_KenshiTogether.info) for Kenshi's uploader.

  The internal name used to be KenshiCoop; it changed on 2026-10-27 because another
  Workshop mod ships the same KenshiCoop folder and DLL names.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1 -Version 0.66.7

.EXAMPLE
  # Reuse the current build instead of recompiling.
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1 -Version 0.66.7 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    # Release version shown in README/PROVENANCE and in the zip name
    # (major.PROTOCOL.patch: a new protocol bumps the middle number).
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$modName   = "KenshiTogether"

if (-not $SkipBuild) {
    # The PLAYER release ships the Release config: the shipped DLL excludes the
    # scenario harness and does not define KENSHICOOP_HARNESS. The test pipeline
    # uses Harness instead.
    Write-Host "=== build plugin (Release / shipped, no scenario harness) ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" Release"
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

$dll  = Join-Path $repoRoot "src\plugin\x64\Release\$modName.dll"
$src  = Join-Path $repoRoot "dist\mods\$modName"
$json = Join-Path $src "RE_Kenshi.json"
$mod  = Join-Path $src "$modName.mod"
foreach ($f in $dll, $json, $mod) { if (-not (Test-Path $f)) { throw "missing: $f" } }

# Canonical shipped-DLL hash: package from ONE DLL and assert the packaged copy is
# byte-identical to it, so the release artifact's SHA-256 is verifiable.
$canonSha = (Get-FileHash -Algorithm SHA256 $dll).Hash
Write-Host "Canonical Release DLL SHA-256: $canonSha"

# Rebuild dist\mod-kit from scratch so no stale file survives.
$kitDir = Join-Path $repoRoot "dist\mod-kit"
$modDir = Join-Path $kitDir $modName
if (Test-Path $kitDir) { Remove-Item -Recurse -Force $kitDir }
New-Item -ItemType Directory -Force -Path $modDir | Out-Null

Write-Host "=== assembling the $modName mod folder ==="
Copy-Item $dll  (Join-Path $modDir "$modName.dll")
Copy-Item $json (Join-Path $modDir "RE_Kenshi.json")
Copy-Item $mod  (Join-Path $modDir "$modName.mod")
# Protocol 65: the plugin reads its kit version from here (next to the DLL) for
# the version check message ("the host has 0.65.0, you have 0.64.1").
Set-Content (Join-Path $modDir "version.txt") $Version -Encoding ASCII -NoNewline

# coop_config.json (LAN/UDP only; Steam play needs no config). Written fresh so
# the release always ships a clean default.
@'
{
  // Kenshi Together config. For a normal Steam game you do NOT need to edit this
  // file: hosting and joining are done in-game (press F2).
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
  // %LOCALAPPDATA%\kenshi\KenshiTogether_backups\<save>\<date_time>\ - copy one
  // back into your save folder to restore it.
  "saveBackups": 3,
  // HOST only: what the other players may do. Also switchable live in the F2
  // panel while hosting. true = allowed.
  "joinCanBuild": true,
  "joinCanUseContainers": true,
  "joinCanBuy": true
}
'@ | Set-Content (Join-Path $modDir "coop_config.json") -Encoding UTF8

# One-click installer (scripts\installer): finds Kenshi, installs RE_Kenshi (and
# the 1.0.65 executable it runs on a 1.0.68 install), installs + enables the mod,
# and uninstalls. It sits at the kit root and installs the mod folder next to it.
$installerDir = Join-Path $scriptDir "installer"
foreach ($f in "$modName-Setup.cmd", "$modName-Setup.ps1") {
    Copy-Item (Join-Path $installerDir $f) (Join-Path $kitDir $f)
}

# Top-level README (sibling to the mod folder, so it is NOT copied into the game).
@'
Kenshi Together __VERSION__ - co-op for Kenshi
==============================================

INSTALL (every player)
----------------------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it anywhere.
  2. Close Kenshi, then double-click KenshiTogether-Setup.cmd.
     It finds your Kenshi, installs RE_Kenshi (the free loader the co-op plugin
     needs; downloaded from github.com/BFrizzleFoShizzle/RE_Kenshi and checked
     against a fixed SHA-256), installs the KenshiTogether mod and enables it.
     On Kenshi 1.0.68 it also creates the 1.0.65 game executable RE_Kenshi
     runs on - your own 1.0.68 executable is not modified.
  3. Start Kenshi normally. The main menu should read "RE_Kenshi 0.3.5".

  Kenshi somewhere other than a Steam library? Run it from a command prompt:
     KenshiTogether-Setup.cmd -KenshiDir "D:\Games\Kenshi"

  Manual install: install RE_Kenshi 0.3.1+ yourself
  (https://www.nexusmods.com/kenshi/mods/847), copy the "KenshiTogether" folder
  into <Kenshi>\mods\ and enable "KenshiTogether" in Kenshi's Mods menu.

PREREQUISITES (every player)
----------------------------
  1. Kenshi 1.0.65 or 1.0.68 on Steam.
  2. Steam RUNNING and ONLINE. No port forwarding, no IP addresses.
  3. Everyone installs the SAME Kenshi Together release (the game refuses a
     different version) and the same enabled mods.

PLAY
----
  Press F2 for the co-op panel (main menu or in game).

  HOST - load your save or start a new game ("Multiplayer Wanderer" in
  the New Game list is a ready two-squad start), press F2 and click
  "Host for friends". Your game is now open to your Steam friends.
  "Invite a Steam friend" also sends them a Steam invite.

  JOIN - start Kenshi and stay at the MAIN MENU (no save needed). Press F2:
  under "Join a friend" every Steam friend who is hosting is listed - click
  "Join <name>'s game". The host's world is sent to you and loads by itself.
  A new player can edit their character (race, gender, look) in Kenshi's own
  editor; everyone sees the result after one automatic resync.

  Each player controls their own squad tab, named after their Steam name, and
  gets the same tab back next time. The host's save is the shared save: only
  the host saves. A player who leaves keeps their squad in the world as
  "<name> (away)": it stands still and takes no melee damage until they come
  back. If the host closes the game, the other players get a message and
  Kenshi restarts to its main menu.

  Only the HOST pauses and picks the game speed; other players can request a
  speed or a pause from the panel. Players can give items to each other; taking
  from another player's character asks its owner first, unless the host turns
  on "Players may take from each other" (panel > Host settings).

  Something out of sync? "Resync" in the panel reloads everyone from the host's
  world. Found a bug? "Report" puts your logs in a folder on your Desktop.

PLAY (LAN / direct UDP - advanced)
----------------------------------
  Open <Kenshi>\mods\KenshiTogether\coop_config.json in Notepad, set
  "transport": "udp", and put the host's address in "ip" (and "port" if you
  changed it). In the panel open "Advanced (IP / Steam ID)", pick Host/Join and
  go ONLINE.

UNINSTALL
---------
  KenshiTogether-Setup.cmd -Uninstall                  removes the mod
  KenshiTogether-Setup.cmd -Uninstall -RemoveReKenshi  also removes RE_Kenshi, if
                                                       the installer put it there
  The installer kept a backup of every file it edited (*.kenshicoop.bak).

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiTogether'; reinstalling RE_Kenshi
    usually fixes it.
  * No friends listed under "Join a friend": both Steams must be RUNNING and
    ONLINE, and the host must have clicked "Host for friends".
  * "version mismatch": one player has an older/newer release; everyone should
    use the same one.
  * Logs: <Kenshi>\KenshiTogether_host.log / KenshiTogether_join.log.
'@ | Set-Content (Join-Path $kitDir "README.txt") -Encoding UTF8
$readme = Join-Path $kitDir "README.txt"
(Get-Content $readme -Raw).Replace('__VERSION__', $Version) | Set-Content $readme -Encoding UTF8

# Turkish install + test guide for players (tracked in docs/, shipped as-is).
$trGuide = Join-Path $repoRoot "docs\KURULUM_TR.txt"
if (Test-Path $trGuide) { Copy-Item $trGuide (Join-Path $kitDir "KURULUM_TR.txt") }

# Provenance: assert the PACKAGED DLL is byte-identical to the canonical build,
# then record the hash next to the kit so the release artifact is verifiable.
$packagedSha = (Get-FileHash -Algorithm SHA256 (Join-Path $modDir "$modName.dll")).Hash
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

# Zip: the mod folder, the installer, the READMEs and PROVENANCE.json.
Get-ChildItem (Join-Path $repoRoot "dist") -Filter "$modName-v*.zip" | Remove-Item
$zip = Join-Path $repoRoot "dist\$modName-v$Version.zip"
Compress-Archive -Path (Join-Path $kitDir "*") -DestinationPath $zip

# Steam Workshop content: the mod folder as-is. A Workshop item lands in
# steamapps\workshop\content\233860\<id>\ - the plugin finds its files next to
# its own DLL, so no path in it depends on mods\.
$wsDir = Join-Path $repoRoot "dist\workshop\$modName"
if (Test-Path $wsDir) { Remove-Item -Recurse -Force $wsDir }
New-Item -ItemType Directory -Force -Path $wsDir | Out-Null
Copy-Item (Join-Path $modDir "*") $wsDir
# Kenshi's own uploader (the FCS) reads the picture and the item record from the
# mod folder: _<mod>.img (any PNG/JPG) and _<mod>.info (id 0 = a new item;
# visibility 0 public / 1 friends / 2 private). After the first upload the FCS
# writes the item id into the .info - copy it back to dist\workshop so every
# later upload updates the same item.
Copy-Item (Join-Path $repoRoot "dist\workshop\preview.png") (Join-Path $wsDir "_$modName.img")
Copy-Item (Join-Path $repoRoot "dist\workshop\_$modName.info") (Join-Path $wsDir "_$modName.info")

Write-Host ""
Write-Host "Kit zipped: $zip"
Write-Host "Workshop folder: $wsDir"
