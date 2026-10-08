// Each spec's main jobs, most important first: a pass by hand (notes/SPEC_JOBS_PASS.md, local). Kept by hand;
// tools/gen_spec_builds.py only prints the log audit to check it against.
//
// Rows are read top to bottom and their jobs appended (without repeats) when the row's condition holds for the
// player over tonight, so a build's jobs can sit between the spec's own:
//   ""              always
//   "Ventari", "Mallyx", "Jalis", "not Ventari"   Revenant legends used ("Legendary ... Stance" buffs)
//   "Wanderer's"    Firebrand damage at 0.4x the squad average or more (Minstrel's Firebrands stay under 0.31x,
//                   Wanderer's over 0.46x: 67 player-evenings)
//   "damage build", "support build", "hybrid build"   Chronomancer: damage and stability or quickness against the
//                   squad average, 1.5x or more (130 rounds: 93 support, 27 damage, 5 hybrid)
//   "power", "support"   specs Snow Crows lists with both a power and a support build (reference/snowcrows,
//                   2026-09-29): damage at 0.6x the squad average or more is the power build (player-evenings sit at
//                   0.8x+ or under 0.4x on 18 nights); no rounds to judge by: power
// Job names are the Review measure names (UiReview.cpp, OutputRows). The first job sets the role: Healing /s =
// healer, Stability to subgroup = stability support, Strips /min = stripper, Damage to players /s = damage.
// struct SpecJobs is declared in src/UiCommon.h

#define DAMAGE "Damage to players /s"
#define HEALING "Healing /s"
#define BARRIER "Barrier /s"
#define CLEANSES "Cleanses /min"
#define STRIPS "Strips /min"
#define CC "CC on enemies /min"
#define REVIVES "Rounds with a revive skill"
#define NEGATED "Attacks negated /min"
#define DISTORTION "Distortion timed with enemy spikes"
#define DOWNS "Down contribution"
// A build's key skill on time (the Snow Crows guides): "<skill> in our spike" or "<skill> before enemy spikes", by the
// skill's name in the log (UiReview.cpp, KeySkillOf)
#define IN_OUR_SPIKE(skill) skill " in our spike"
#define BEFORE_THEIRS(skill) skill " before enemy spikes"
#define ON_SUBGROUP(boon) boon " to subgroup"

const SpecJobs kSpecJobs[] = {
	// Guardian
	{"Guardian", "", {DAMAGE}},
	{"Dragonhunter", "power", {DAMAGE, IN_OUR_SPIKE("Procession of Blades")}},
	{"Dragonhunter", "support", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Aegis"), CLEANSES, HEALING}},
	{"Firebrand", "", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Quickness"), ON_SUBGROUP("Aegis"), CC, ON_SUBGROUP("Resolution"), BEFORE_THEIRS("\"Stand Your Ground!\"")}},
	{"Firebrand", "Wanderer's", {DAMAGE}},
	{"Willbender", "", {DAMAGE}},
	{"Luminary", "", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Protection"), ON_SUBGROUP("Resistance"), ON_SUBGROUP("Quickness"), ON_SUBGROUP("Aegis")}},
	// Revenant: the legends decide much of the job
	{"Revenant", "", {DAMAGE}},
	{"Herald", "", {DAMAGE, ON_SUBGROUP("Fury")}},
	{"Herald", "Jalis", {ON_SUBGROUP("Stability")}},
	{"Renegade", "", {ON_SUBGROUP("Might")}},
	{"Renegade", "Mallyx", {STRIPS}},
	{"Renegade", "", {DAMAGE, ON_SUBGROUP("Resistance")}},
	{"Renegade", "Jalis", {ON_SUBGROUP("Stability")}},
	{"Vindicator", "Ventari", {HEALING, BARRIER, CLEANSES}},
	{"Vindicator", "not Ventari", {DAMAGE}},
	{"Vindicator", "Mallyx", {STRIPS}},
	{"Vindicator", "Jalis", {ON_SUBGROUP("Stability")}},
	{"Conduit", "Ventari", {HEALING, BARRIER, CLEANSES}},
	{"Conduit", "not Ventari", {DAMAGE}},
	{"Conduit", "Mallyx", {STRIPS}},
	{"Conduit", "Jalis", {ON_SUBGROUP("Stability")}},
	// Warrior
	{"Warrior", "", {DAMAGE}},
	{"Berserker", "", {DAMAGE}},
	{"Spellbreaker", "power", {DAMAGE, STRIPS, IN_OUR_SPIKE("Winds of Disenchantment")}},
	{"Spellbreaker", "support", {ON_SUBGROUP("Might"), STRIPS, ON_SUBGROUP("Protection"), ON_SUBGROUP("Resistance")}},
	{"Bladesworn", "", {DAMAGE}},
	{"Paragon", "", {ON_SUBGROUP("Resistance"), ON_SUBGROUP("Might"), ON_SUBGROUP("Stability"), ON_SUBGROUP("Protection"), ON_SUBGROUP("Quickness"), REVIVES, ON_SUBGROUP("Vigor"), BEFORE_THEIRS("Banner of Tactics")}},
	// Engineer
	{"Engineer", "", {DAMAGE}},
	{"Scrapper", "power", {DAMAGE}},
	{"Scrapper", "support", {CLEANSES, HEALING}},
	{"Holosmith", "", {DAMAGE}},
	{"Mechanist", "", {DAMAGE}},
	{"Amalgam", "", {DAMAGE}},
	// Ranger
	{"Ranger", "", {DAMAGE}},
	{"Druid", "", {HEALING, CLEANSES, ON_SUBGROUP("Regeneration"), REVIVES, ON_SUBGROUP("Resistance")}},
	{"Soulbeast", "", {DAMAGE}},
	{"Untamed", "", {DAMAGE, STRIPS}},
	{"Galeshot", "", {DAMAGE}},
	// Thief
	{"Thief", "", {DAMAGE}},
	{"Daredevil", "", {DAMAGE}},
	{"Deadeye", "", {DAMAGE}},
	{"Specter", "", {HEALING, BARRIER, ON_SUBGROUP("Stability"), ON_SUBGROUP("Protection"), CLEANSES, ON_SUBGROUP("Resistance"), ON_SUBGROUP("Vigor")}},
	{"Antiquary", "", {DAMAGE}},
	// Elementalist
	{"Elementalist", "", {DAMAGE}},
	{"Tempest", "power", {DAMAGE, DOWNS}},
	{"Tempest", "support", {CLEANSES, ON_SUBGROUP("Protection"), HEALING, REVIVES}},
	{"Weaver", "", {DAMAGE}},
	{"Catalyst", "power", {DAMAGE}},
	{"Catalyst", "support", {CLEANSES, HEALING, ON_SUBGROUP("Quickness"), ON_SUBGROUP("Protection"), ON_SUBGROUP("Vigor"), REVIVES}},
	{"Evoker", "power", {DAMAGE}},
	{"Evoker", "support", {HEALING, CLEANSES, ON_SUBGROUP("Regeneration"), ON_SUBGROUP("Protection"), REVIVES}},
	// Mesmer
	{"Mesmer", "", {DAMAGE}},
	{"Chronomancer", "damage build", {DAMAGE}},
	{"Chronomancer", "support build", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Quickness"), CC, ON_SUBGROUP("Resistance"), IN_OUR_SPIKE("Continuum Split")}},
	{"Chronomancer", "hybrid build", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Quickness"), DAMAGE, CC}},
	{"Mirage", "", {STRIPS, DAMAGE}},
	{"Virtuoso", "", {DAMAGE}},
	// Resistance: Singularity Shot always, Harmonious Harp only with Mental Defense
	{"Troubadour", "", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Quickness"), ON_SUBGROUP("Resistance"), BARRIER, HEALING, ON_SUBGROUP("Protection"), DISTORTION, NEGATED, ON_SUBGROUP("Vigor"), IN_OUR_SPIKE("Crescendo")}},
	// Necromancer
	{"Necromancer", "", {STRIPS, DAMAGE, IN_OUR_SPIKE("Well of Corruption"), IN_OUR_SPIKE("Nightfall")}},
	{"Reaper", "", {STRIPS, DAMAGE, DOWNS, IN_OUR_SPIKE("Well of Corruption"), IN_OUR_SPIKE("Nightfall")}},
	{"Scourge", "", {STRIPS, DAMAGE, IN_OUR_SPIKE("Well of Corruption"), IN_OUR_SPIKE("Nightfall")}},
	{"Harbinger", "power", {DAMAGE, IN_OUR_SPIKE("Well of Corruption")}},
	{"Harbinger", "support", {ON_SUBGROUP("Might"), ON_SUBGROUP("Fury"), STRIPS, ON_SUBGROUP("Quickness"), IN_OUR_SPIKE("Well of Corruption")}},
	{"Ritualist", "power", {DAMAGE, CC}},
	{"Ritualist", "support", {ON_SUBGROUP("Quickness"), ON_SUBGROUP("Stability"), ON_SUBGROUP("Might"), ON_SUBGROUP("Protection"), CLEANSES}},
};

#undef DAMAGE
#undef HEALING
#undef BARRIER
#undef CLEANSES
#undef STRIPS
#undef CC
#undef REVIVES
#undef NEGATED
#undef DISTORTION
#undef DOWNS
#undef IN_OUR_SPIKE
#undef BEFORE_THEIRS
#undef ON_SUBGROUP
