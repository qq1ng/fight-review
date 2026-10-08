#pragma once
// What decided a round, from the log. A round is won or lost on downs, so everything here is about one down at a time:
// who went down, what state they were in at that moment, and what state everyone else under the same fire was in (the
// risk set). Comparing the downed player with the allies hit at the same moment who stayed up holds the enemy's pressure
// fixed: what differs between them is what made the difference. The same for enemy downs (what allies did to the enemies
// they downed against the enemies they hit who stayed up), for pushes, and for what became of each down (got up or died,
// and the revive tools there were for it).
//
// Shared by the addon's "why" view and the offline analysis that set its numbers (one definition for both).

#include <array>
#include <cstdint>
#include <vector>

#include "Analysis.h"

namespace Causes
{
	using Analysis::Fight;
	using Analysis::Player;

	constexpr int32_t kLookBack = 3000;   // the moment before a down: its last 3 s
	constexpr int32_t kStripLookBack = 4000; // a strip counts a little further back: it opens the way for the CC and the damage
	constexpr float kNear = 600;          // "near": within 600 units (a revive's reach on foot, a melee ball)

	// An ally's state at a moment (Ms), from what the log shows in the kLookBack before it
	struct AllyState
	{
		int Player = -1;
		bool Case = false;                // the one who went down at this moment
		int Hits = 0, Enemies = 0;        // enemy strikes that landed, distinct enemies they came from
		int64_t Damage = 0;               // their damage (health and barrier)
		int Cc = 0;                       // CC that landed on them (a landed CC means no stability right then)
		int32_t CcMs = 0;                 // how long it held them, summed
		int32_t SinceCc = -1;             // ms from the last CC that landed to the moment (-1: none in the window)
		bool StabAt = false;              // stability on them half a second before
		bool StabStripped = false;        // the enemy took all their stability at once (strip, corrupt) in kStripLookBack
		int StabUsedUp = 0;               // stability stacks the enemy took one at a time (CC blocked, partial strip)
		int Strips = 0, Corrupted = 0;    // boons the enemy took (corrupted: turned into a condition)
		int32_t HardMs = 0, ImmobMs = 0;  // time under immobilize / chill / cripple / fear; immobilize alone
		bool Protection = false, Aegis = false, Resistance = false, Regeneration = false, Quickness = false;
		int HpBefore = -1;                // health (% x 100) at the start of the window, -1 unknown
		int64_t Healed = 0;               // healing they got (Healing Stats players only)
		int Dodges = 0, Evaded = 0, Negated = 0, StunBreaks = 0;
		float ToTag = -1, ToSquad = -1;   // distance to the commander, to the squad's middle (-1 unknown)
		float Ahead = 0;                  // how much nearer the enemy's middle than the squad's middle is (+ ahead, - behind)
		float AheadBefore = -99999;       // the same at the window's start (kLookBack before), -99999 unknown: there before the burst?
		// Who moved in the window (-99999 unknown): this player toward the enemy's middle, the squad's middle toward it, the
		// enemy's middle toward this player (each as it stood at the window's start); and a teleport on them (a pull, a blink)
		float MovedIn = -99999, SquadMovedIn = -99999, EnemyMovedIn = -99999;
		bool Teleported = false;
		int Pulled = 0;                   // CC in the window that moved them toward the enemy: a pull, a taunt (or a knockback-or-pull not told apart)
		int AlliesNear = 0, EnemiesNear = 0; // within kNear, up
		int SubgroupStab = -1;            // % of their subgroup (others, up) with stability half a second before; -1 none
		bool InEnemySpike = false;
	};

	// An enemy's state at a moment: what allies did to them in the kLookBack before it
	struct EnemyState
	{
		int Enemy = -1;
		bool Case = false;
		int Hits = 0, Allies = 0;         // ally strikes, distinct allies they came from
		int64_t Damage = 0;
		int Cc = 0;                       // ally CC that landed on them
		int StabBlocked = 0;              // their stability stacks ally CC used up
		bool StabStripped = false;        // allies stripped their stability (kStripLookBack)
		int Strips = 0;
		int32_t HardMs = 0, ImmobMs = 0;  // ally hard conditions on them; immobilize alone
		int HpBefore = -1;
		int Invulnerable = 0;             // seconds of the window with an ally hit on them absorbed (distortion and the like)
		float ToTheirSquad = -1;          // distance to the enemy's own middle
		float Ahead = 0;                  // how much nearer the ally squad's middle than their own middle is (+ out in front)
		int AlliesNear = 0, EnemiesNear = 0; // allies / their own side within kNear, up
		bool InAllySpike = false;
	};

	// One down and the risk set: everyone on that side up and under fire in the kLookBack before it (the downed one
	// included, Case = true)
	struct AllyDown
	{
		int32_t Ms = 0;
		int Player = -1;
		std::vector<AllyState> Risk;
		// what became of it
		bool Died = false;                // went from downed to dead
		int32_t DownMs = 0;               // how long they were down
		int64_t Cleave = 0;               // enemy damage on them while down
		int Reviving = 0;                 // allies who started reviving them by hand
		bool Rallied = false;             // got up within 0.4 s of an enemy they had hit in the 15 s before dying
		bool RevivedBySkill = false;      // a revive skill or Illusion of Life picked them up
		int AlliesNear = 0, EnemiesNear = 0; // up within kNear at the down
		int ToolsReady = -1;              // revive tools ready among carriers up within reach (-1: no night context)
		std::vector<int> Ready;           // those carriers (Players indexes)
		int ToolsReadyStrict = -1;        // the same on a strict reading: within 1200 (cast range), the full recharge (no alacrity)
		int ToolsSpentBefore = 0;         // revive tool uses in this round before it, with nobody down in reach
		bool InEnemySpike = false;
		int Spike = -1;                   // the enemy spike run it fell in (index into TheirSpikesMs), -1 none
		bool AfterStop = false;           // after allies had stopped fighting (Round::AllyStopMs): not counted in the causes
	};
	struct EnemyDown
	{
		int32_t Ms = 0;
		int Enemy = -1;
		std::vector<EnemyState> Risk;
		bool Died = false;
		int32_t DownMs = 0;
		int64_t Cleave = 0;               // ally damage on them while down
		int Spike = -1;                   // the ally spike run it fell in, -1 none
		// how it ended when it didn't die: rallied (an ally they had hit in the 15 s before died within 0.4 s of them getting
		// up: in GW2 a downed player gets up when a foe they damaged is defeated), else picked up (their revives and skills
		// aren't in the log: the rest); EndOfLog: still down when the log ended
		bool Rallied = false, EndOfLog = false;
		int Hitters = 0;                  // allies who hit them while down
		int32_t FirstHit = -1;            // ms from the down to the first ally hit on it, -1 none
		int64_t CleaveEarly = 0;          // ally damage in its first 1.5 s (the same time for every down: how hard, not how long)
		int HittersEarly = 0;             // allies who hit it in its first 1.5 s
		bool AfterStop = false;           // after the enemy had stopped fighting (Round::EnemyStopMs)
	};

	// One push (a spike run): what came before and in it, and what it did
	struct Push
	{
		bool Ally = true;                 // an ally push (enemy downs) or an enemy push (ally downs)
		int Index = -1;                   // into OurSpikesMs / TheirSpikesMs
		int32_t First = 0, Last = 0, Peak = 0;
		int Downs = 0, Deaths = 0;        // the other side's, from 1 s before it to 3 s after
		int Up = 0, Hitting = 0;          // the pushing side up at its peak, of them hitting a player of the other side
		int DamageUp = 0, DamageHitting = 0; // the same for damage players (ally pushes)
		int64_t Damage = 0;               // to the other side's players in it
		double TopTargetShare = 0;        // share of that damage on the one most hit
		int Targets = 0;                  // players of the other side it hit
		double Sync = 0;                  // share of its damage in the peak second and those next to it
		double Focus = 0;                 // share of its damage on a player 10+ of the pushing side hit within 1.5 s of it
		int CcBefore = 0, CcIn = 0;       // CC that landed on the other side in the 3 s before its start, in it
		int Blocked = 0;                  // CC the other side's stability took in it and the 3 s before (ally pushes)
		int StabStripsBefore = 0, StripsBefore = 0; // the other side's stability / boons removed in the 3 s before
		int Immobilized = 0;              // the other side's players immobilized at its peak (ally pushes)
		int Wells = 0;                    // Well of Corruption / Suffering cast from 3 s before to 1.5 s after its start
		int Negated = 0;                  // the pushing side's hits the other side negated in it (ally pushes: absorbed)
		float Gap = -1;                   // distance between the two sides' middles at its start
		// enemy pushes: the hit allies at its peak
		int HitAllies = 0, HitWithStab = 0, HitStripped = 0, HitCcd = 0;
	};

	struct Round
	{
		std::vector<AllyDown> AllyDowns;
		std::vector<EnemyDown> EnemyDowns;
		std::vector<Push> Pushes;         // ally then enemy, each in time order
		// When each side stopped fighting for good, ms from fight start (-1: to the end): from there its damage on the other's
		// players, summed over 5 s, stayed under 35% of its fighting level. The downs after it came once the round was decided
		int32_t AllyStopMs = -1, EnemyStopMs = -1;
	};

	// The night around the round, for revive tools: the rounds in time order and this one's place (tools used in an
	// earlier round may still be recharging). Empty: no tools counted.
	struct Night
	{
		std::vector<const Fight*> Rounds;
		int Index = -1;
	};

	// aRiskSets false: each down's own state only, no one else's (all the view needs; the analysis wants the sets)
	Round Compute(const Fight& f, const Night& aNight = {}, bool aRiskSets = true);

	// ---- The round in numbers, for the "why" view ----
	// Why allies went down: what the downed ally had in the 3 s before, and how strongly each went with going down against
	// allies under the same fire (odds ratios from 6,275 downs in 550 fights, 3 Sept to 6 Oct; each one's own effect with
	// the others held fixed). Strips and how many enemies hit them are left out on purpose: they are how the enemy's spike
	// works (a strip matters by letting the CC land, so it is told inside the CC line). Few allies near was tested and left
	// out: with positions known it went slightly the other way (0.70: the middle of a ball takes more AoE). Out ahead is
	// told by how they got there in the 3 s ("ahead at the down" was 9.4, but 87% of those allies weren't ahead 3 s before;
	// not the tag, not the role): CC'd during the kite (left behind as the squad pulled back while CC'd, rooted or slowed:
	// 95-98% of those left behind; the few free to move went down less), or ran past the squad on their own (a pull or taunt
	// past it is CC, counted there).
	enum Factor { F_Cc, F_Immob, F_NoProt, F_CcKite, F_RanPast, F_Hurt, F_Count };
	bool HasFactor(const AllyState& s, int aFactor, const Fight& f);

	// Fight sizes. CC weighs more the bigger the fight (3.5 at 15v15, 5.8 at 40+), protection less (3.8 to 2.6), the same
	// inside each player's logs. The odds and the view's won-fight values for each size, from every player's logs at hand:
	// tools/causal/gen_refs.py writes src/CauseRefs.inc. A line's values: won fights' median and worse quartile (counts:
	// the upper one; shares and rates: the lower), what the cause cost in a won fight (for the order), lost fights'
	// median.
	struct LineRef { double Median = 0, Worse = 0, Cost = 0, Lost = 0; };
	constexpr int kRefLines = 14; // the view's lines, in UiStruggle.cpp's LineId order
	struct SizeBand
	{
		int MaxAllies = 0;                   // up to this many allies in the round (the last band: any more)
		const char* Name = "";               // "15v15"
		int Fights = 0, WonFights = 0;       // behind the odds (every period); behind the won-fight values (this patch)
		std::array<double, F_Count> Odds{};  // each cause's odds ratio, the others held fixed
		std::array<LineRef, kRefLines> Lines{};
		LineRef Downs, Died, EnemyDowns, EnemyDied; // the parts' heads: downs per 10 allies per minute, shares that died
	};
	const SizeBand& BandOf(int aAllies);

	// One enemy push's ally downs (the downs in its spike run): where a round turned
	struct Wave
	{
		int Push = -1;                    // into TheirSpikesMs
		int32_t From = 0, To = 0;         // its run
		int Downs = 0, Deaths = 0, Cc = 0, StabStripped = 0;
		int ToolsReady = -1;              // the fewest revive tools ready in reach at its downs
		int Subgroup = 0, SubgroupDowns = 0; // the subgroup it hit most
		std::vector<int> Downed;          // AllyDowns indexes
	};

	struct Summary
	{
		int Downs = 0, Deaths = 0;
		std::array<int, F_Count> With{};      // downs with each factor
		std::array<double, F_Count> Cost{};   // downs each factor cost: a down's excess risk shared among its factors by log odds
		std::vector<uint8_t> Flags;           // per AllyDowns entry: the factors it had (bit per Factor)
		int AfterStopDowns = 0, AfterStopDeaths = 0; // after allies stopped fighting: told apart, in no line (Downs and Deaths hold them)
		int32_t AllyStopMs = -1;
		int CcStabStripped = 0;               // CC'd downs whose stability was stripped first
		int32_t CcToDownMs = -1;              // the median time from the CC to the down
		// what became of the downs
		int DiedFast = 0, DiedRanOut = 0, DiedUnused = 0; // in under 1.5 s; 1.5 s+ with 0 or 1 revive tool ready in reach; with 2+
		std::vector<int> UnusedBy;            // carriers who had a tool ready in reach of a down that died (Players, most first)
		int ToolUses = 0, ToolsOnNobody = 0;
		int32_t LastToolMs = -1;
		std::vector<std::pair<int, int32_t>> OnNobody; // (Players index, ms)
		// ally pushes and enemy downs
		int EnemyDowns = 0, EnemyDeaths = 0, AllyPushes = 0, PushesWithDowns = 0;
		double Focus = -1, CcLanded = -1, CcBefore = -1; // focus share, ally CC that landed of all thrown, CC before each push
		int64_t CleaveMedian = -1;            // ally damage on an enemy down, median
		// finishing them, time held equal
		double HittersEarly = -1;             // allies who hit an enemy down in its first 1.5 s, the middle down (2+ downs)
		int64_t CleaveEarly = -1;             // their damage on it then, the middle down
		int EnemyRallied = 0;                 // enemy downs that got up the moment an ally they had hit died
		std::vector<Wave> Waves;              // enemy pushes that downed allies, most downs and deaths first
	};
	Summary Summarize(const Fight& f, const Round& r);

	// Helpers shared with the view
	bool UpAt(const Player& p, int32_t aMs);                     // not downed or dead
	bool OnAt(const std::vector<std::pair<int32_t, int32_t>>& aSpans, int32_t aMs);
	int32_t TimeIn(const std::vector<std::pair<int32_t, int32_t>>& aSpans, int32_t aFrom, int32_t aTo);
	int SpikeAt(const Fight& f, bool aOurs, int32_t aMs);        // the run holding aMs (1 s margin), -1 none
	// when a side stopped fighting for good (Round::AllyStopMs), from its damage per second (ToPlayersPerS or InPerS); -1 none
	int32_t StopMs(const std::vector<int64_t>& aPerS);
	int64_t StampSeconds(const Fight& f);                        // the log's end as seconds since 2000, from its name
	// a revive tool's WvW recharge at a moment (ms since 2000, as StampSeconds * 1000; the default: today), 0 if not one.
	// From the patch notes (tools/patchnotes.py): Illusion of Life 75 s until 2025-02-11, 90 s since.
	int32_t ReviveRecharge(int32_t aSkill, int64_t aAtMs = INT64_MAX);
	bool IsReviveTool(const Player& p, int32_t aSkill);          // one of the tools the revive order counts
}
