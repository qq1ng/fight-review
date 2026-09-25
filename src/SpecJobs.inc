// Each spec's main jobs, most important first: the user's pass of 2026-09-24 (notes/SPEC_JOBS_PASS.md, local). Kept by hand;
// tools/gen_spec_builds.py only prints the log audit to check it against.
//
// Rows are read top to bottom and their jobs appended (without repeats) when the row's condition holds for the
// player over tonight, so a build's jobs can sit between the spec's own:
//   ""              always
//   "Ventari", "Mallyx", "Jalis", "not Ventari"   Revenant legends used ("Legendary ... Stance" buffs)
//   "Wanderer's"    Firebrand damage at 0.4x the squad average or more (Minstrel's Firebrands stay under 0.31x,
//                   Wanderer's over 0.46x: 67 player-evenings, 2026-09-24)
//   "damage build", "support build", "hybrid build"   Chronomancer: damage and stability or quickness against the
//                   squad average, 1.5x or more (130 rounds: 93 support, 27 damage, 5 hybrid)
// Job names are the Review measure names (UiReview.cpp, OutputRows). The first job sets the role: Healing /s =
// healer, Stability on subgroup = stability support, Strips /min = stripper, Damage to players /s = damage.
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
#define ON_SUBGROUP(boon) boon " on subgroup"

const SpecJobs kSpecJobs[] = {
	// Guardian
	{"Guardian", "", {DAMAGE}},
	{"Dragonhunter", "", {DAMAGE}},
	{"Firebrand", "", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Quickness"), ON_SUBGROUP("Aegis"), CC, ON_SUBGROUP("Resolution")}},
	{"Firebrand", "Wanderer's", {DAMAGE}},
	{"Willbender", "", {DAMAGE}},
	{"Luminary", "", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Protection"), ON_SUBGROUP("Quickness"), ON_SUBGROUP("Aegis")}},
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
	{"Spellbreaker", "", {DAMAGE, STRIPS}},
	{"Bladesworn", "", {DAMAGE}},
	{"Paragon", "", {ON_SUBGROUP("Resistance"), ON_SUBGROUP("Might"), ON_SUBGROUP("Quickness"), ON_SUBGROUP("Vigor")}},
	// Engineer
	{"Engineer", "", {DAMAGE}},
	{"Scrapper", "", {CLEANSES, HEALING}},
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
	{"Specter", "", {HEALING, BARRIER, ON_SUBGROUP("Stability"), ON_SUBGROUP("Protection"), CLEANSES, ON_SUBGROUP("Vigor")}},
	{"Antiquary", "", {DAMAGE}},
	// Elementalist
	{"Elementalist", "", {DAMAGE}},
	{"Tempest", "", {CLEANSES, ON_SUBGROUP("Protection"), HEALING, REVIVES}},
	{"Weaver", "", {DAMAGE}},
	{"Catalyst", "", {ON_SUBGROUP("Protection"), CLEANSES, ON_SUBGROUP("Vigor"), REVIVES}},
	{"Evoker", "", {DAMAGE}},
	// Mesmer
	{"Mesmer", "", {DAMAGE}},
	{"Chronomancer", "damage build", {DAMAGE}},
	{"Chronomancer", "support build", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Quickness"), CC, ON_SUBGROUP("Resistance")}},
	{"Chronomancer", "hybrid build", {ON_SUBGROUP("Stability"), ON_SUBGROUP("Quickness"), DAMAGE, CC}},
	{"Mirage", "", {STRIPS, DAMAGE}},
	{"Virtuoso", "", {DAMAGE}},
	{"Troubadour", "", {ON_SUBGROUP("Stability"), BARRIER, HEALING, ON_SUBGROUP("Protection"), DISTORTION, NEGATED, ON_SUBGROUP("Vigor")}},
	// Necromancer
	{"Necromancer", "", {STRIPS, DAMAGE}},
	{"Reaper", "", {STRIPS, DAMAGE}},
	{"Scourge", "", {STRIPS, DAMAGE}},
	{"Harbinger", "", {DAMAGE}},
	{"Ritualist", "", {DAMAGE}},
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
#undef ON_SUBGROUP
