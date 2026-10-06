<!-- The release text (.github/workflows/release.yml). Before each tag, rewrite "What's new" with only what changed
since the last release, not the base features. Keep "Install". This comment doesn't show on GitHub. -->
## What's new

- **New layout**: the round's debrief stays on the left (result, spikes, you, downs, enemy, best players, calls);
  whatever you click opens on the right, with Back. A small 250 px window shows the lines you pick while you play.
- **Spikes**: a long push is one spike now ("0:02-0:14"), not several 3 s apart; its downs count together.
- **Round graph**: ally invulnerability with the enemy hits it absorbed; hover a spike for what happened, click to
  open it.
- **Calls**: the wells as a grid per Necromancer (who left one out), the Warrior opener as a row of icons, Tale and
  stability as tables (stability per subgroup at each enemy spike: who had it, the CC it took, who went down).
- **Enemy**: fighting more than one server, the enemy count is split by team.
- **Fixes**: Chronomancer bursts timed by their hits; a cancelled or lone well isn't a call; Signet of Might judged by
  its effect; damage totals leave out pets.

## Install

Put `FightReview.dll` into `<Guild Wars 2>\addons\`, then start the game and press Ctrl+Shift+F.

Needed:
- [Nexus](https://raidcore.gg/Nexus)
- ArcDPS (from the Nexus addon library), with WvW logs turned on: ArcDPS options (Alt+Shift+T), Logging, save WvW logs
- Healing Stats (recommended, from the Nexus addon library): without it your healing shows as "unknown"

Step by step, with a check for each: see the [README](https://github.com/qq1ng/fight-review#install).
