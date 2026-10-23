Kenshi Together 0.59.0 - co-op for Kenshi (mod folder: KenshiCoop)
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

