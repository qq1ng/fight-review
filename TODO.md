# TODO

## Boon attribution: which skill gave a boon (needs a proper fix)

The log says who applied a boon to whom and when, not which skill did it. Today (`src/Analysis.cpp`,
`AttributeBoon`) a boon goes to a skill the player was using at that moment (or up to 0.75 s before) if either:

- the GW2 API's skill facts say the skill gives that boon (`tools/gen_removal_skills.py` -> `src/BoonSkills.inc`), or
- this round's log shows it: pooled over everyone of the same profession, the boon followed at least half of the
  skill's 3+ uses, at least 3 times as often as chance plus one (`Fight::LearnedGivers`).

Everything else goes to "other sources (traits, relics, sigils, runes, combos)".

Known gaps:

- [ ] **Too few uses per round.** Skills used once or twice a round never reach 3 uses, even pooled over a
  profession. Pool the evidence over all rounds of the evening (and keep it between sessions, e.g. a learned
  table in `addons/FightReview/`), then attribute with that. Needs attribution after the evidence is in, so
  either a second pass over earlier rounds or a table carried from round to round.
- [ ] **Boons given a while after the use.** Chapter 4: Stalwart Stand gave resistance on 5 of 5 uses but
  stability on only 2 of 5 within 0.75 s: its stability probably comes later or on a condition. Find out when
  (look at the delay from each use to the next stability from that Firebrand) and allow per-skill delays.
- [ ] **Uses the log doesn't show.** Mesmer shatters F1-F3 (no buff, no cast event) and other instant skills
  without an animation, a buff of their name, or a hit or heal of their own. Decode the effect events
  (EFFECTAGENTCREATE / EFFECTGROUNDCREATE and IDTOGUID, as Elite Insights' rules do) to find those uses.
- [ ] **Trait effects.** A trait that grants a boon when a skill is used shows up as that skill giving the boon
  (fine for "which of my skills gave it"), but a trait that grants boons on its own (on crit, on dodge, every
  N seconds) stays under "other sources". Consider naming the likely trait from the GW2 API's `/v2/traits`.
- [ ] **API traited facts.** The API list includes boons a skill gives only with a trait (e.g. Tome of Courage's
  stability needs trait 612), whether or not the player took it. The logs don't record traits; the learned
  evidence could veto API entries that never show up in a player's log.
- [ ] **Check against real play.** Ask the user which of their skills give which boons in their build, and
  compare with what `frcheck` prints as "learned" for their rounds.

## Other open items

- [ ] Python prototype (`tools/`) doesn't have the C++-only features: cleanse/strip/boon attribution rules,
  stability performance, the new use detection. `tools/check_cpp.py` now differs on cast counts by design.
- [ ] Stability "Redundancy" from TopStats' stability performance isn't built.
- [ ] Logs load from the last 8 hours only; consider an option to load older logs.
