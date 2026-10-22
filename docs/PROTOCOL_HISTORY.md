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
