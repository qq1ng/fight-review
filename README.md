# Fight Review

A Guild Wars 2 addon for WvW. After each fight it reads the log ArcDPS saved and shows what happened: who went
down and why, how the spikes went on both sides, and how you did on your build against others playing the same.

Everything comes from the saved logs, after the fight. Nothing is shown while you fight.

![Summary](docs/images/summary.png)

## What it shows

- **Summary**: the round at a glance, and one thing to work on next round.
- **You**: your build's main jobs against the best player on the same spec, and what to fix first.
- **Deaths**: every down in the round, what led to it, and whether a revive came.
- **Round**: both sides' damage over the round, every spike, and a breakdown of each spike.
- **Squad**: each subgroup and player: boons, stability, healing, cleanses, downs.
- **Compare**: you next to another player, skill by skill.

Most tabs can show this round or the whole evening. Ctrl+Shift+F opens the window; it is also in the Nexus quick
access menu.

| | |
|---|---|
| ![You](docs/images/you.png) | ![Deaths](docs/images/deaths.png) |
| ![Round](docs/images/round.png) | ![Enemy spike](docs/images/enemy-spike.png) |

## Install

Fight Review is in beta. `<GW2>` is your Guild Wars 2 folder. Download the latest `FightReview.dll` from
[Releases](../../releases/latest).

1. **Nexus**: install it from [raidcore.gg/Nexus](https://raidcore.gg/Nexus) into your GW2 folder.
   Check: `<GW2>\d3d11.dll` exists.
2. **ArcDPS**: in game, open Nexus, Addons, search for ArcDPS and install or enable it (or get it from
   [deltaconnected.com/arcdps](https://www.deltaconnected.com/arcdps/)).
   Then turn on WvW logs: ArcDPS options (Alt+Shift+T), Logging, save WvW logs.
   Check: after a fight, a new `.zevtc` file appears in `Documents\Guild Wars 2\addons\arcdps\arcdps.cbtlogs\WvW`.
3. **Healing Stats** (recommended): in Nexus, Addons, search for Healing Stats and install it (or download it from
   its [releases page](https://github.com/Krappa322/arcdps_healing_stats/releases) into `<GW2>\addons`).
   Without it your healing shows as "unknown".
   Check: `<GW2>\addons\arcdps\arcdps.log` has a line with `extensions: healing_stats`.
4. **Fight Review**: put `FightReview.dll` into `<GW2>\addons\`.
   Check: `<GW2>\addons\Nexus\Nexus.log` has `Loaded addon: FightReview.dll`.

Start the game and press Ctrl+Shift+F. At start the addon loads the logs of your last play session, and after that
each new log a few seconds after ArcDPS saves it. Before the first fight, the window shows which log folder it is
watching. It keeps its settings in `<GW2>\addons\FightReview\`.

## Good to know

- Logs stay on your PC. The addon downloads skill icons from the official GW2 API.
- Some things are estimates, because the log doesn't record them (which traits a player took, for example). The
  addon says so where it matters.

## Built on

- [Nexus](https://raidcore.gg/Nexus) by Raidcore: the addon loader, its addon API and Mumble headers.
- [ArcDPS](https://www.deltaconnected.com/arcdps/) by deltaconnected: the combat logs everything here is read from.
- [Healing Stats](https://github.com/Krappa322/arcdps_healing_stats) by Krappa322: healing and barrier in the logs.
- [GW2 Elite Insights](https://github.com/baaron4/GW2-Elite-Insights-Parser): its rules for casts the log doesn't
  record, skill names and icons, and the definitions of shared stats.
- [TopStats](https://github.com/Drevarr/GW2_EI_log_combiner): the reference for WvW stats; stability coverage and
  redundancy follow its definitions.
- [Guild Wars 2 API](https://wiki.guildwars2.com/wiki/API:Main): skills, traits, boons and icons.
- [Snow Crows](https://snowcrows.com/): their WvW builds were the reference for each build's main jobs.
- [Dear ImGui](https://github.com/ocornut/imgui) and [miniz](https://github.com/richgel999/miniz).

Licences and the data generated from these sources are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Guild Wars 2 is a trademark of ArenaNet, LLC; this project is not affiliated with ArenaNet or NCSOFT.

## Building from source

Visual Studio 2022 with the C++ desktop workload. Run `scripts\build.ps1`; the DLL ends up in `build\release\`.
