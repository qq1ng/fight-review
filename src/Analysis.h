#pragma once
// One fight's review, computed from its log. C++ port of the Python prototype in tools/ (fight_stats.py,
// boons.py, skills.py, timeline.py); the definitions and their checks against TopStats are in notes/HANDOFF.md (local).

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

	// Which skills give which boons, as seen in logs: for a skill, how many uses, after how many of them the player
	// applied each boon, and how many a random moment would show. Kept per player (their build decides what a skill
	// gives) and per profession (the fallback for skills a player rarely used; key: profession << 32 | skill), and
	// pooled over rounds (Session keeps it between sessions), so a skill used once a round still gets learned.
	// After / AfterExpected (per profession only): boon moments in each second after a use ends, and what the
	// player's usual rate would give. A pulsing skill (Hallowed Ground) keeps them well above usual for seconds.
	constexpr int kLagSeconds = 6;
	struct BoonEvidence
	{
		int Uses = 0;
		std::array<int, kBoons> Followed{};
		std::array<double, kBoons> Expected{};
		std::array<std::array<int, kLagSeconds>, kBoons> After{};
		std::array<std::array<double, kLagSeconds>, kBoons> AfterExpected{};
	};
	struct Evidence
	{
		std::map<uint64_t, BoonEvidence> ByProfession;
		std::map<std::pair<std::string, int32_t>, BoonEvidence> ByPlayer; // (account, skill)
	};
	void AddEvidence(Evidence& aTo, const Evidence& aFrom, int aSign = 1);

	struct SkillRow
	{
		int     Casts = 0;
		int     Hits = 0;          // heal, barrier and damage events (a boon's or trait's ticks when nothing was cast)
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

	// Revenant legends, seen as the "Legendary ... Stance" buff on the player (their build: Ventari heals, Mallyx strips)
	enum Legend : uint32_t { L_Centaur = 1, L_Demon = 2, L_Dwarf = 4, L_Assassin = 8, L_Dragon = 16, L_Renegade = 32, L_Alliance = 64, L_Entity = 128 };
	uint32_t LegendOf(const std::string& aBuffName); // 0: not a legend
	bool UsableCharacterName(const std::string& aName); // false: a placeholder or a WvW rank name

	// Crowd control by type. ArcDPS puts the type on the CC hit (Elite Insights' "ArcDPSGeneric" skill ids); a stun
	// and a daze share one, told apart by the Stun or Daze effect applied with it, and so do a knockback and a pull,
	// told apart by whether the player ended up nearer the enemy who did it.
	enum CcKind : uint8_t { CC_Stun, CC_Daze, CC_StunOrDaze, CC_Knockdown, CC_Knockback, CC_Pull, CC_KnockbackOrPull, CC_Launch,
		CC_Float, CC_Fear, CC_Taunt, CC_Stagger, CC_Other, CC_Count };
	extern const std::array<const char*, CC_Count> kCcVerbs; // "stunned", "knocked down"
	extern const std::array<const char*, CC_Count> kCcIcons; // the icon's name: "Stun", "Knockdown"

	struct Player
	{
		uint64_t    Addr = 0;
		std::string Name, Account, Spec, Profession; // Name: what the UI shows (see Fight::AccountNames)
		std::string Character;         // the character name as the log has it
		uint32_t    ProfId = 0, EliteId = 0; // for icons
		int         Subgroup = 0;
		bool        Pov = false;       // whose log this is: "you"
		bool        HealKnown = false; // their own Healing Stats reported their heals
		int64_t     ActiveMs = 0;      // fight time not dead
		int64_t     Heal = 0, HealDowned = 0, Barrier = 0, Damage = 0, DamageAll = 0;
		int         Strips = 0, Cleanses = 0, CleansesSelf = 0, Evades = 0, Blocks = 0, Invulns = 0;
		int         Downs = 0, Deaths = 0;
		int         CcTaken = 0;       // times crowd controlled
		int         CcDealt = 0;       // crowd control landed on anything hostile (TopStats "appliedCrowdControl")
		uint32_t    Legends = 0;       // Legend bits: Revenant legends used
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
		std::vector<int32_t> CleanseMs, StripMs;            // when they removed an ally's condition, an enemy's boon
		// What happened to this player, for the stability view and the cause-of-death review (ms from fight start)
		// Enemy: index in Fight::Enemies. Barrier: the part of Damage that barrier took (strikes; the rest hit health)
		struct TakenHit { int32_t Ms, Skill, Damage; int Enemy = -1; int32_t Barrier = 0; };
		std::vector<TakenHit> HitsIn;                        // strikes from enemies
		std::vector<TakenHit> HitsOut;                       // this player's damage to enemy players (Enemy: who), for the spike breakdown
		std::vector<std::pair<int32_t, int32_t>> HealsIn;    // (ms, healing received)
		// Boons the enemy removed from this player. Corrupted: turned into a condition (a condition from the same enemy
		// within 10 ms: a third of the strips in the user's logs), else stripped.
		struct StripHit { int32_t Ms; int Boon; int Enemy = -1; bool Corrupted = false; };
		std::vector<StripHit> StripsIn;
		struct CcHit { int32_t Ms; int32_t Duration; CcKind Kind; int Enemy = -1; }; // Enemy: index in Fight::Enemies
		std::vector<CcHit> CcIn;                             // each CC that landed on this player, with its type
		std::vector<int32_t> DodgeMs;                        // dodges (the Dodge skill's animation)
		// Illusion of Life on this player: a Mesmer's revive that lasts 15 s; unless they rally, they go back down when
		// it ends. By: index in Fight::Players (-1 unknown); RanOut: it ended at its full length
		struct Illusion { int32_t From = 0, To = 0; int By = -1; bool RanOut = false; };
		std::vector<Illusion> IllusionOfLife;
		std::vector<TakenHit> EvadedIn;                      // enemy strikes this player evaded (Damage: 0)
		// Elite Insights' down contribution: damage this player did to enemy players from their last 90% health to a
		// down that led to their death
		int64_t     DownContribution = 0;
		int         StabStripped = 0;  // times the enemy removed all of this player's stability at once
		int         StabUsedUp = 0;    // stability stacks the enemy removed one at a time (CC blocked, or a partial strip)
		int         StabGivenLost = 0; // stability stacks this player gave that the enemy removed
		std::vector<std::pair<int32_t, int>> StabLost;   // (ms, 0 stripped all at once / 1 a stack used up)
		// Each moment this player gave stability: when, the skill it's credited to, and whom it reached (indices into
		// Fight::Players, the giver included). Pulses to several allies within 150 ms are one moment.
		struct StabGive { int32_t Ms = 0; int32_t Skill = 0; std::vector<int> Targets; };
		std::vector<StabGive> StabGives;
		std::array<std::vector<std::pair<int32_t, int32_t>>, kBoons> BoonOn; // merged spans with the boon, any giver
		std::vector<std::pair<int32_t, int32_t>> Hp;         // (ms, health % x 100)
		struct Point { int32_t Ms; float X, Y; };
		std::vector<Point> Pos;                              // positions
		std::vector<int32_t> TeleportMs;                     // teleports (blinks, portals, pulls)
		// Reviving: the plain revive (skill 1066, which names the ally), and completed revive skills (Illusion of
		// Life, Signet of Mercy, Glyph of Renewal, Signet of Undeath, Spirit of Nature, a Paragon's Battle Standard)
		struct Revive { int32_t From, To; int Target; };    // Target: index in Fight::Players
		std::vector<Revive> Reviving;
		int64_t     ReviveMs = 0;                           // time spent reviving allies (EI: "Time Resurrecting")
		struct ReviveUse
		{
			int32_t Ms, Skill;
			int     DownNear = 0, GotUp = 0; // allies downed within reach then; of those, up within 3 s
			bool    Done = true;             // false: the cast stopped short
			uint8_t Stop = 0;                // why it stopped (n_animationstop: 8 interrupted, 9 died, 10 downed, 11 CC'd)
		};
		std::vector<ReviveUse> ReviveUses;
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
		struct Enemy
		{
			std::string Spec;
			std::vector<Player::Point> Pos;
			std::vector<std::pair<int32_t, int32_t>> Hp; // (ms, health % x 100)
			std::vector<Span> DownSpans;                 // downed (Dead false) and dead (Dead true), as for players
		};
		std::vector<Enemy>    Enemies;   // enemy players who hit us or had a position: spec, positions, health, downs
		int                   SquadDowns = 0, SquadDeaths = 0, EnemyDowns = 0, EnemyDeaths = 0;
		int64_t               SquadDamage = 0, EnemyDamage = 0; // damage dealt by each side (players and minions)
		std::vector<Player>   Players;    // present squad members
		int                   Pov = -1;   // index into Players
		bool                  PovAbsent = false; // the recorder is in the squad but took no part in this round
		int                   Commander = -1; // index into Players: who had a commander tag
		std::array<bool, kBoons> Intensity{};
		// Uptime per subgroup: [subgroup][boon] average stacks (intensity) or % (queued); key 0 = squad
		std::map<int, std::array<double, kBoons>> GroupUptime;
		std::map<int, int> GroupSize;
		std::map<int, int64_t> GroupDamageTaken; // damage the members of each subgroup took
		std::vector<int64_t> OurSpikesMs, TheirSpikesMs;     // from fight start
		std::vector<int32_t> OurStripsMs, OurCcMs;           // enemy boons we removed, CC we landed on enemy players
		std::vector<int32_t> OurCleansesMs;                  // conditions we removed from allies
		std::vector<int64_t> SupportPerS;                    // our healing and barrier per second (Healing Stats players only)
		std::vector<int64_t> OutPerS, InPerS;                 // damage per second
		std::vector<int64_t> ToPlayersPerS;                   // our damage to enemy players per second (our spikes)
		std::array<double, T_Count> TimingBaseline{};         // share of fight seconds in each class
		std::vector<int32_t> SquadDownMs, EnemyDownMs;        // each down, ms from fight start
		// CC windows on each subgroup's members and how many had anyone's stability (TopStats' coverage, any giver)
		std::map<int, int> GroupCcWindows, GroupCcCovered;
		// Skills the log showed giving boons (profession, skill, bit mask in kBoonNames order), beyond the GW2 API's facts
		struct LearnedGiver { std::string Account; uint32_t Profession; int32_t Skill; uint32_t Boons; }; // per player
		std::vector<LearnedGiver> LearnedGivers;
		Evidence OwnEvidence; // this round's part of the evidence behind LearnedGivers
		struct Pulse { uint32_t Profession; int32_t Skill; int Boon; int32_t Ms; }; // learned: keeps giving the boon this long after a use
		std::vector<Pulse> Pulses;
		std::map<int32_t, std::string> SkillNames;
		uint32_t MapId = 0;
		// Players are named by account in Edge of the Mists (968), where other worlds' players have no character name
		// (a placeholder like "ag1458" or their WvW rank, "Mithril Champion"), and whenever a squad name looks like that
		// elsewhere; by character name in normal WvW (the Rezz Order rule, 2026-09-24)
		bool AccountNames = false;

		double SquadGeneration(const Player& aPlayer, int aBoon) const;
		// Same over the player's own subgroup (TopStats "Group Generation"): stacks or % of its uptime
		double GroupGeneration(const Player& aPlayer, int aBoon) const;
	};

	// Throws on unreadable logs.
	// aOthers: evidence from other rounds, learned from together with this round's own (nullptr: this round only).
	// aOthersHaveThis: aOthers already holds this round's evidence (it was counted before), so it isn't added twice.
	Fight Analyse(const std::filesystem::path& aPath, const Evidence* aOthers = nullptr, bool aOthersHaveThis = false);
}
