# Kenshi Together

Co-op multiplayer for **[Kenshi](https://lofigames.com/)**, built as an
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) plugin.

One player hosts their world; friends join from Kenshi's main menu over Steam and
each plays their own squad inside it. Characters, NPCs, combat, inventories,
trades, ground items, buildings, containers, research, one shared money pool and
game speed are kept in sync, and the host's save is the shared save.

> **Status: beta.** Played and tested regularly with 2-3 players. Expect the odd
> desync - the in-game **Resync** and **Report** buttons exist for exactly that.

Kenshi Together is a fork of [KenshiCoop by nhoral](https://github.com/nhoral/KenshiCoop).
Its files are named `KenshiTogether` so it never collides with other mods built on
the same base.

## Play

Every player needs **Kenshi 1.0.65 or 1.0.68 (Steam)** with **Steam running and
online**. No port forwarding, no IP addresses.

1. **Install** (every player): extract the release zip, close Kenshi and
   double-click **`KenshiTogether-Setup.cmd`**. It installs
   [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) (the plugin loader) and
   the `KenshiTogether` mod, and enables it. (`-Uninstall` removes it again.)
2. **Host:** load a save or start a new game ("Multiplayer Wanderer" is a ready
   two-squad start), press **F2** and click **Host for friends**.
3. **Join:** start Kenshi and stay at the main menu. Press **F2** and click
   **Join &lt;name&gt;'s game**. The host's world is sent over and loads by itself.

The kit's `README.txt` (and `KURULUM_TR.txt` in Turkish) has the full guide:
character editing, host settings, Resync/Report, LAN play and troubleshooting.

### Good to know

- **Your own squad.** Each player controls their own squad tab, named after their
  Steam name, and gets the same tab back next session.
- **New characters.** A joining player can edit their character (race, gender,
  look) in Kenshi's own editor; one automatic resync shows it to everyone.
- **Only the host saves, pauses and sets the speed.** Others can request a speed
  or a pause from the panel.
- **Trading.** Give items freely; taking from another player's character asks its
  owner first (the host can allow free taking).
- **Leaving.** A player who leaves keeps their squad in the world as
  "&lt;name&gt; (away)". If the host closes the game, everyone else is told and
  Kenshi restarts to its main menu.
- **Two co-op starts** come with the mod: "Multiplayer Wanderer" (vanilla
  Wanderer with two wanderers in separate squads, by
  [zeroit789](https://github.com/zeroit789)) and "Multiplayer+ Wanderer" (the
  same with 500,000 cats in the shared wallet and every stat at 50).

## How it works

- `KenshiTogether.dll` is loaded into the game by RE_Kenshi. It hooks the engine
  via KenshiLib and drives all game mutation on the main thread.
- Networking is [ENet](https://github.com/lsalzman/enet) over UDP through a Steam
  P2P tunnel (or plain UDP for LAN).
- The host is authoritative for the world; each player is authoritative for their
  own squad. See `docs/API_REFERENCE.md` and `docs/PROTOCOL_HISTORY.md`.

```
src/plugin/       The plugin (net, sync/replication, engine facade, test scenarios)
src/netproto/     Shared wire-protocol headers (plain C++03)
src/netsim/       WAN simulator (delay/jitter/loss proxy) for tests
src/prototest/    Wire-protocol unit tests
src/tunneltest/   Steam-tunnel socket-hook tests
dist/mods/        Packaging source: RE_Kenshi.json + the .mod (co-op starts)
scripts/          Build, deploy, installer, kit and automated-test tooling
tools/            MultiplayerStartGen (builds the .mod) and analysis helpers
docs/             Build guide, engine/API reference, protocol history
third_party/      ENet patches, VC10 compat shim (deps are fetched, not committed)
```

## Building

The plugin must be compiled with the **Visual C++ 2010 (v100) x64 toolset** (a
KenshiLib requirement). Toolchain setup is in [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).

```bash
cmd //c scripts/build_plugin.cmd Release
powershell -ExecutionPolicy Bypass -File scripts/make_mod_kit.ps1 -Version 0.66.7
```

`make_mod_kit.ps1` builds the release zip (`dist/KenshiTogether-v<version>.zip`)
and the Steam Workshop content folder (`dist/workshop/KenshiTogether/`).

Dependencies are fetched, not committed:

- KenshiLib + precompiled libs: clone
  [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  into `third_party/KenshiLib_deps/`
- ENet: clone [lsalzman/enet](https://github.com/lsalzman/enet) into
  `third_party/enet/enet/` and apply the patches in `third_party/enet/patches/`

## Development and testing

`scripts/run_test.ps1` launches two local Kenshi installs (host + join), runs a
named scenario and judges it from both logs; `scripts/regress.ps1` runs the
scenario suite. See [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md) Parts D-E.

## Credits

- [nhoral](https://github.com/nhoral) - KenshiCoop, the base this fork builds on
- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) - RE_Kenshi and KenshiLib
- [lsalzman/enet](https://github.com/lsalzman/enet) - UDP networking library
- [zeroit789](https://github.com/zeroit789) - the "Multiplayer Wanderer" start
- Lo-Fi Games - Kenshi

## License

[AGPL-3.0](LICENSE). KenshiLib and RE_Kenshi are GPLv3; this plugin links KenshiLib
under GPLv3 section 13 (GPL/AGPL combination). Not affiliated with Lo-Fi Games.
Non-commercial fan project.
