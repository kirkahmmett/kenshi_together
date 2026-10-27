# KENSHI TOGETHER — CO-OP MULTIPLAYER

**One world. Separate squads. Shared suffering.**

We're a few friends who wanted to experience the world of
[Kenshi](https://lofigames.com/) together. So we decided to build the co-op mod we
wished existed. We figured we probably weren't the only ones, so we decided to share
it with everyone.

> **Kenshi Together is currently in BETA and actively being developed.** Expect bugs,
> regular updates and improvements as we continue playing and building alongside the
> community.

## Features

- Explore and survive together in one shared world.
- Control your own characters and independent squads.
- Synchronized combat, NPCs, items, trading, buildings and research.
- Shared economy and persistent world using the host's save.
- Join through Steam friends. No IPs or port forwarding.
- In-game **F2** panel for multiplayer controls, pings and troubleshooting.

## Installation

**From the Steam Workshop**

1. Subscribe to Kenshi Together on the Workshop.
2. Install [RE_Kenshi 0.3.1+](https://www.nexusmods.com/kenshi/mods/847) separately
   (required plugin loader).
3. Enable Kenshi Together in your Kenshi mod list.
4. Make sure everyone has the same mod versions and Steam is online.

**From the zip** ([Releases](https://github.com/kirkahmmett/kenshi_together/releases))

1. Extract the zip, close Kenshi and double-click `KenshiTogether-Setup.cmd`.
   It installs RE_Kenshi and the mod for you (`-Uninstall` removes them again).
2. Use either the Workshop item or the zip - not both.

## How to play

- **HOST:** Start a new game → Press **F2** → **Host for friends**.
- **JOIN:** Stay at the main menu → Press **F2** → **Join** → Select your friend's session.
- The host's world loads automatically. Each player controls their own squad.

## Beta & feedback

Currently tested with up to 3 players. Bugs and desyncs may happen!

- **Desync?** F2 → **Resync**. Give reality a gentle shove.
- **Found a bug?** F2 → **Report**. Send us the evidence and we'll track down the culprit.
- **Got ideas?** Open an [issue](https://github.com/kirkahmmett/kenshi_together/issues)
  or drop them in the Workshop discussions. Even the terrible ones. This is Kenshi,
  after all.

## Credits & transparency

Built upon [KenshiCoop by nhoral](https://github.com/nhoral/KenshiCoop). Huge thanks
for laying the foundation that made this project possible! Also thanks to
[BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) for RE_Kenshi and KenshiLib,
[lsalzman/enet](https://github.com/lsalzman/enet) for networking,
[zeroit789](https://github.com/zeroit789) for the "Multiplayer Wanderer" start, and
Lo-Fi Games for Kenshi.

**Developed entirely with AI.** We're just gamers and game development enthusiasts
learning the craft as we go. AI helped us turn an idea into something we could
actually play, test and share.

We know AI isn't everyone's thing, and that's perfectly fair. Roast us, skip the mod,
or give it a shot. No hard feelings either way. The technology is here, whether we
like it or not. We figured we'd have more fun exploring what we can build with it
than arguing about whether it should exist.

---

## For developers

`KenshiTogether.dll` is an [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) plugin. It hooks the engine
and drives all game changes on the main thread. Networking is
[ENet](https://github.com/lsalzman/enet) over a Steam P2P tunnel (or plain UDP for LAN).
The host is authoritative for the world; each player for their own squad. See
`docs/API_REFERENCE.md` and `docs/PROTOCOL_HISTORY.md`.

**Building** needs the Visual C++ 2010 (v100) x64 toolset (a KenshiLib requirement);
setup is in [docs/BUILD_SETUP.md](docs/BUILD_SETUP.md).

```bash
cmd //c scripts/build_plugin.cmd Release
powershell -ExecutionPolicy Bypass -File scripts/make_mod_kit.ps1 -Version 0.66.7
```

This makes the release zip (`dist/KenshiTogether-v<version>.zip`) and the Workshop
folder (`dist/workshop/KenshiTogether/`). Dependencies are fetched, not committed:
[KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
into `third_party/KenshiLib_deps/`, and [ENet](https://github.com/lsalzman/enet) into
`third_party/enet/enet/` with the patches in `third_party/enet/patches/`.

**Testing:** `scripts/run_test.ps1` runs two local Kenshi installs (host + join) through
a named scenario and judges both logs; `scripts/regress.ps1` runs the suite.

## License

[AGPL-3.0](LICENSE). KenshiLib and RE_Kenshi are GPLv3; this plugin links KenshiLib
under GPLv3 section 13. Not affiliated with Lo-Fi Games. Non-commercial fan project.
