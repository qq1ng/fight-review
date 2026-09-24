#pragma once
// One fight's review, computed from its log. C++ port of the Python prototype in tools/ (fight_stats.py,
// boons.py, skills.py, timeline.py); the definitions and their checks against TopStats are in HANDOFF.md.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "Evtc.h"

namespace Analysis
{
	// TopStats' boon order
	constexpr int kBoons = 12;
	extern const std::array<uint32_t, kBoons> kBoonIds;
	extern const std::array<const char*, kBoons> kBoonNames;
	constexpr int kStability = 8;

	enum Timing { T_IntoOurs, T_AheadOfTheirs, T_AnsweringTheirs, T_Count };
	extern const std::array<const char*, T_Count> kTimingNames;

	struct SkillRow
	{
		int     Casts = 0;
		int64_t Heal = 0;
		int64_t Barrier = 0;
		int64_t Damage = 0;        // on enemy players
		int64_t DamageAll = 0;     // on anything hostile
		int     Strips = 0;        // attributed to a skill that removes boons, else 0 (other sources)
		int     Cleanses = 0;      // attributed to a skill that removes conditions, else 0 (other sources)
		int     Interrupted = 0;   // casts stopped before their trigger point by CC, a down or death
		int     Cancelled = 0;     // casts the player stopped before their trigger point (other skill, dodge)
		std::array<double, kBoons> BoonSquadS{};  // attributed: seconds given to the rest of the squad
		std::array<double, kBoons> BoonGroupS{};  // attributed: seconds given to the own subgroup
		std::array<int, T_Count> Timing{};        // casts in each timing class
		std::vector<int32_t> CastMs;              // each cast's time from fight start
	};

	struct Span { int32_t From = 0, To = 0; bool Dead = false; }; // ms from fight start

	struct Player
	{
		uint64_t    Addr = 0;
		std::string Name, Account, Spec, Profession;
		uint32_t    ProfId = 0, EliteId = 0; // for icons
		int         Subgroup = 0;
		bool        Pov = false;       // whose log this is: "you"
		bool        HealKnown = false; // their own Healing Stats reported their heals
		int64_t     ActiveMs = 0;      // fight time not dead
		int64_t     Heal = 0, HealDowned = 0, Barrier = 0, Damage = 0, DamageAll = 0;
		int         Strips = 0, Cleanses = 0, CleansesSelf = 0, Evades = 0, Blocks = 0, Invulns = 0;
		int         Downs = 0, Deaths = 0;
		int         CcTaken = 0;       // times crowd controlled
		int64_t     CcTakenMs = 0;     // total CC duration
		int64_t     DamageTaken = 0;
		int64_t     HealToSelf = 0, HealToGroup = 0, HealToOthers = 0; // Heal split by target
		// Stability when the own subgroup got crowd controlled (TopStats' Stability Performance): CC windows
		// on the subgroup while this player could act, how many had this player's stability on the target,
		// and how many of those still had 3+ s of it left
		int         StabEligible = 0, StabCovered = 0, StabReady = 0;
		int64_t     StabAllyMs = 0, StabSelfMs = 0; // stability stack presence given to others / self
		// Time line: when this player was crowd controlled, downed or dead, had stability, dealt and healed
		std::vector<int32_t> CcMs;             // CC hits taken, ms from fight start
		int         CcNoStab = 0;              // of those, with no stability on the player in the 3 s before
		std::vector<Span> DownSpans;           // downed (Dead false) and dead (Dead true) spans
		int64_t     DownedMs = 0;              // time downed, dead excluded
		std::vector<std::pair<int32_t, int32_t>> StabOnMe; // merged spans with any stability on the player
		std::vector<int32_t> DamagePerS, HealPerS;          // per second of the fight
		std::array<double, kBoons> BoonSquadS{}; // seconds given to the rest of the squad
		std::array<double, kBoons> BoonGroupS{}; // seconds given to the own subgroup, self excluded
		std::map<int32_t, SkillRow> Skills;      // 0 = no cast (traits, relics, sigils) for attributed values

		double PerMin(double aValue) const { return ActiveMs > 0 ? aValue * 60000.0 / ActiveMs : 0.0; }
		// EI squad generation as TopStats shows it: stacks (intensity) or % (queued), averaged over the others
	};

	struct Fight
	{
		std::filesystem::path Path;
		std::string           Stamp;      // yyyymmdd-hhmmss from the file name (the fight's end)
		int64_t               DurationMs = 0;
		int                   SquadCount = 0, EnemyCount = 0;
		int                   SquadDowns = 0, SquadDeaths = 0, EnemyDowns = 0, EnemyDeaths = 0;
		int64_t               SquadDamage = 0, EnemyDamage = 0; // damage dealt by each side (players and minions)
		std::vector<Player>   Players;    // present squad members
		int                   Pov = -1;   // index into Players
		std::array<bool, kBoons> Intensity{};
		// Uptime per subgroup: [subgroup][boon] average stacks (intensity) or % (queued); key 0 = squad
		std::map<int, std::array<double, kBoons>> GroupUptime;
		std::map<int, int> GroupSize;
		std::map<int, int64_t> GroupDamageTaken; // damage the members of each subgroup took
		std::vector<int64_t> OurSpikesMs, TheirSpikesMs;     // from fight start
		std::vector<int64_t> OutPerS, InPerS;                 // damage per second
		std::array<double, T_Count> TimingBaseline{};         // share of fight seconds in each class
		std::vector<int32_t> SquadDownMs, EnemyDownMs;        // each down, ms from fight start
		// CC windows on each subgroup's members and how many had anyone's stability (TopStats' coverage, any giver)
		std::map<int, int> GroupCcWindows, GroupCcCovered;
		// Skills the log showed giving boons (profession, skill, bit mask in kBoonNames order), beyond the GW2 API's facts
		struct LearnedGiver { uint32_t Profession; int32_t Skill; uint32_t Boons; };
		std::vector<LearnedGiver> LearnedGivers;
		std::map<int32_t, std::string> SkillNames;

		double SquadGeneration(const Player& aPlayer, int aBoon) const;
		// Same over the player's own subgroup (TopStats "Group Generation"): stacks or % of its uptime
		double GroupGeneration(const Player& aPlayer, int aBoon) const;
	};

	// Throws on unreadable logs.
	Fight Analyse(const std::filesystem::path& aPath);
}
