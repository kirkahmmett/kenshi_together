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
