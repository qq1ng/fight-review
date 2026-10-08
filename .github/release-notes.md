<!-- The release text (.github/workflows/release.yml). Before each tag, rewrite "What's new" with only what changed
since the last release, not the base features. Keep "Install". This comment doesn't show on GitHub. -->
## What's new

- **Why it was lost / won**: a new view that ranks what cost you players this round: CC'd with no stab, immobilized,
  no protection, CC'd while the squad kited, running past the squad, coming in hurt, rez skills not used. Each is
  compared with won fights of the same size (15v15 up to 40v40+), with who it hit.
- **The reset doesn't count**: downs after the squad stopped fighting (the call to reset, the rest getting picked
  off) are shown apart and left out of the reasons.
- **Where it turned**: the enemy pushes that cost the most, with the CC, strips and rez skills ready in each.
- **Left side**: Round, Why, You, Calls, Downs, Enemy, Best. Round shows CC blocked and CC landed against your
  night; clicking YOU or your rank opens You fresh.
- **Calls**: Elementalist, Chronomancer and Necromancer bursts and Druid heals show their skills as icons; Tale
  sync is judged stricter.
- **Fixes**: logs from before May 2026 now show boons and conditions; non-English game clients read correctly;
  damage to breakbars no longer counts as damage.

## Install

Put `FightReview.dll` into `<Guild Wars 2>\addons\`, then start the game and press Ctrl+Shift+F.

Needed:
- [Nexus](https://raidcore.gg/Nexus)
- ArcDPS (from the Nexus addon library), with WvW logs turned on: ArcDPS options (Alt+Shift+T), Logging, save WvW logs
- Healing Stats (recommended, from the Nexus addon library): without it your healing shows as "unknown"

Step by step, with a check for each: see the [README](https://github.com/qq1ng/fight-review#install).
