# Protocol history (this fork)

Upstream kept its version-by-version history in `resources/PROTOCOL_HISTORY.md`,
which is git-ignored and was never published; protocols 1-55 are described only
by the comments in `src/netproto/Wire.h`. This file starts at the first bump made
in this fork. Every `PROTOCOL_VERSION` change adds an entry here.

A version mismatch is refused at the handshake: **every player must run the same
build** after a bump.

## 56 - host settings for what a join may do (issue #42)

- New packet `PKT_PERMS = 49` / `PermsPacket` (8 bytes): `seq`, `joinBuild`,
  `joinContainers`, `joinBuy` (1 = allowed). Host -> join, `CH_RELIABLE`.
- The host sends it on the connect edge, whenever a switch changes in its F2
  panel, and every 10 s as a safety resend. The join keeps the newest `seq`
  (reset when the host leaves, so a restarted host numbers from 1 again).
- The JOIN enforces it locally in the engine detours: the build-mode commit
  (`PreviewBuilding::placeFinalPreviewBuilding`), `Inventory::buyItem`, and the
  inventory-add veto for moves between its own characters and building storage.
  A refused action logs `[perm] REFUSED ...` and the status banner says why.
- Inbound queue `perms_` is session-preserving (survives a world swap).
- Starting values: `joinCanBuild` / `joinCanUseContainers` / `joinCanBuy` in
  `coop_config.json` (or `KENSHICOOP_JOIN_BUILD` / `_CONTAINERS` / `_BUY`).

## 57 - several joins, player names (three-player groundwork)

- The host relays join-authored state packets to every other join verbatim, the
  author's `ownerId` intact (entity batches, events, inventory, world items,
  medical/stats, buildings, census, transfers, cell claims, deeds, fixtures).
  Requests only the host acts on are not relayed.
- Join ids are the lowest free 1..7 and equal the join's own squad tab, so a
  rejoining player gets their tab back.
- `PKT_PEER_LEFT = 50` (5 bytes, host -> joins): another join left; only the
  host leaving ends a join's session.
- `PKT_ROSTER = 51` (38 bytes, host -> joins): one player's display name. A join
  sends its Steam name in HELLO (`nameLen` + bytes, previously always 0); every
  client labels squad tab <id> with player <id>'s name.
- Steam transport: a host admits every player whose Steam session is accepted
  (no pasted id needed); each gets its own fake ENet address in the tunnel.
- Not on the wire: save receive drains BEGIN/FILE/DONE atomically; players can no
  longer drop items from another player's character.

## 58 - players may not take from each other (host switch)

- `PermsPacket` gains `takeFromPlayers` (9 bytes). Off by default in a real
  session: a player can give items to another player's character but not take
  any out of it (the take is refused at the inventory-add detour and the item
  goes back). The host's "Players may take from each other" switch turns it on.
  Scripted tests keep trading free unless `KENSHICOOP_PLAYERS_TAKE=0`.
- Not on the wire: only the host decides speed and pause (a join's request no
  longer lowers the effective), and trade/dialogue windows no longer pause the
  game for everyone - only Space or the panel's Pause button counts. A player
  who leaves becomes "<name> (away)": frozen and protected from melee damage
  until they return.

## 59 - ready barrier and mod list check (stability)

- `PKT_LOAD_READY` (52) / `LoadReadyPacket` (9 bytes), join -> host: "I loaded
  the world of LOAD_GO `loadId` and am back in gameplay". After every LOAD_GO
  the host holds the game paused for everyone until each connected join has
  reported (90 s timeout; a player who leaves is not waited for). The host
  shows "Paused - waiting for N player(s) to finish loading".
- `WelcomePacket` gains `modHash` + `modCount` (13 bytes): the host's enabled
  mod list (`data\mods.cfg`, lower-cased, in load order, FNV-1a). A join appends
  its own (u32 hash, u16 count) to HELLO after the name. A mismatch only warns
  ("Warning: player N has a different mod list" / "your mods differ from the
  host's") for 20 s and in the log; it never refuses the connection.
- Not on the wire: the inventory detours pass calls from any thread other than
  the game's main thread straight to the engine (Kenshi's loader thread adds
  items while a world loads).

## 60 - character editor for joining players

- `PKT_CHAR_LOOK` (53), variable length: `CharLookHeader` (38 bytes) + name +
  race sid + appearance file. OFFER (host -> joins, broadcast, `target` names
  the player): sent once a player whose squad the host just made (random
  character or a free start character) has finished loading; that player's
  client opens Kenshi's own editor (`PlayerInterface::activateCharacterEditMode`)
  on their character. APPLY (join -> host): when the editor closes, the look -
  `GameData::saveToFile` of the appearance data, race sid, name. The host
  accepts it only for the sender's own squad, loads it into its copy, saves and
  reloads everyone. Not relayed.
- Panel: "Edit my character" reopens the editor at any time (the host's own
  edit also reloads everyone).
- Test lever `KENSHICOOP_LOOK_TEST=N`: a join opens the editor N s into the game
  and closes it 4 s later.

## 61 - resync request

- `PKT_RESYNC_REQ` (54) / `ResyncReqPacket` (5 bytes), join -> host: the panel's
  "Resync" button. The host saves the live world and every client reloads it
  (the coordinated reload a new player's squad uses); at most one every 30 s.
- Not on the wire: "Save a problem report" copies this client's logs, crash
  dumps, mods.cfg and a short report.txt into a Desktop folder.
- Test lever `KENSHICOOP_RESYNC_TEST=N`: a join saves a report and asks for a
  resync N s into the game.

## 62 - consent trade

- `PKT_TRADE_REQ` (55) / `TradeReqPacket` (157 bytes) and `PKT_TRADE_RESP` (56)
  / `TradeRespPacket` (18 bytes), relayed by the host; each client acts only on
  rows whose `target` is its own id. A take the protocol-58 rule refuses (drag
  or the trade window's right-click) becomes a request to the item's owner, who
  accepts or declines in the panel ("Give: X wants Y" / "Decline"). On accept
  the OWNER moves the item (`moveItemBetweenContainers`); the existing
  protocol-37 transfer carries it to the asker's client. `moved` = -2 means the
  asker's character had no room. Requests expire after 120 s; an away
  player's items cannot be requested.
- Test lever `KENSHICOOP_TRADE_TEST=N`: a join asks for an item N s into the
  game; the host accepts every request after 3 s.
