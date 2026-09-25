#pragma once
// Shared parts of the Fight Review window: colours, the per-frame context (round, scope, you, the compared
// player), metrics, drawing helpers (bars, lanes) and the skill detail that Review and Compare both open.
// Design: notes/HANDOFF.md (local), "Redesign agreed 2026-09-24" and "Second redesign".

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "Analysis.h"
#include "Session.h"

namespace Ui
{
	using Analysis::Fight;
	using Analysis::Player;
	using Analysis::SkillRow;
	using Session::FightPtr;

	// ---- colours: sides only (dataviz skill's validator, #141619 surface) ----------------------------------
	extern const ImVec4 kMuted;           // secondary text
	extern const ImU32 kYou, kPeer, kPeerTick, kEnemy, kTrack, kLaneBg, kOurBand, kEnemyBand, kEnemyPre;

	enum Tab { T_You, T_Deaths, T_Compare, T_Round, T_Squad, T_Tonight, T_TabCount };
	enum Role { R_Heal, R_Stab, R_Damage, R_Strip };
	// A common build of a spec, from an audit of the user's logs (src/SpecJobs.inc)
	// A row of a spec's main jobs (src/SpecJobs.inc): appended when When holds for the player ("" = always)
	struct SpecJobs { const char* Spec; const char* When; std::vector<const char*> Jobs; };
	constexpr int32_t kNoSkill = INT32_MIN;

	// ---- state shared by the tabs ----------------------------------------------------------------------------
	struct State
	{
		int         Selected = -1;      // round index, -1 = follow the latest
		bool        AllRounds = false;  // This round / Tonight
		std::string VsAccount;          // "" = the best on your spec
		int         SwitchTo = -1;      // a tab to select next frame
		// Review: breadcrumb level and the open fix line
		int         Measure = -1;       // measure opened by skill, -1 = overview
		int32_t     Skill = kNoSkill;   // skill opened in the drill-down
		int         OpenFix = -1;
		bool        MoreFixes = false;  // all five fixes, not the first three
		bool        AllMeasures = false; // every measure, not only your role's and your spec's jobs
		// Compare
		int         Metric = -1;        // -1 = from your role
		int32_t     CompareOpen = kNoSkill;
		bool        CompareMore[2] = {false, false}; // the folded skills shown: only one used it, both did
		std::string CompareLeft, CompareRight; // Compare's two players by account; "" = you / your compared player
		// Squad
		int         ColumnSet = 0;
		bool        AllBoons = false;
		// Deaths: the revive order
		std::vector<std::string> ReviveOrder; // accounts in your revive order; empty = the usual order (saved)
		// Stability
		int         StabGroup = -1;     // subgroup on the time line, -1 = yours
		// Round
		int         RoundView = 0;      // 0: spikes, 1: stability over time
		std::vector<int32_t> Picked;    // skills marked on the round's time line and drawn in the spike breakdown
		int64_t     SpikeOpen = -1;     // the spike broken down (its time), -1 = the round
		std::string SpikeStamp;         // the round it belongs to
		// Deaths
		std::string DeathKey;           // the down shown: round/account/ms, "" = the first that fits the filter
		int         DeathFilter = 0;    // 0 everyone, 1 you, 2 your subgroup
		int64_t     DeathSpike = -1;    // an enemy spike opened from the Round tab: its downs first
	};
	State& S();
	void SaveSettings(); // the settings file: the summary window, your revive order

	// ---- metrics (per skill) ------------------------------------------------------------------------------------
	enum Kind { K_Amount, K_Count, K_Boon };
	struct Metric
	{
		std::string Name;
		Kind        What;
		std::function<double(const Player&)> Total;
		std::function<double(const SkillRow&)> PerSkill;
		bool        NeedsHealing = false;
		bool        Intensity = false;
	};
	const std::vector<Metric>& Metrics();
	constexpr int kMetricHeal = 0, kMetricBarrier = 1, kMetricDamage = 2, kMetricDamageAll = 3, kMetricStrips = 4,
		kMetricCleanses = 5, kMetricGroupBoon = 6; // + boon index
	const char* RateUnit(const Metric& m);
	double Rate(const Metric& m, const Player& p, double aValue);
	bool Known(const Metric& m, const Player& p);
	int TimingWindow(int aMetric); // Analysis::Timing class that matters for this metric
	const char* WindowLabel(int aWindow);

	// ---- the per-frame context ------------------------------------------------------------------------------------
	struct Ctx
	{
		const std::vector<FightPtr>* Fights = nullptr;
		int Index = 0;
		const Fight* F = nullptr;          // the selected round
		std::vector<FightPtr> Scope;        // this round, or tonight
		bool OneRound = true;
		const Player* MeRaw = nullptr;      // you in F (nullptr: the recorder isn't in the squad)
		const Player* VsRaw = nullptr;      // the compared player in F, if present
		Player You;                         // summed over the scope
		std::vector<Player> Peers;          // same spec (else same role), summed, best first by the role metric
		const Player* Vs = nullptr;         // into Peers
		bool SameSpec = true;               // false: peers are your role on other specs
		int BestOwnRound = -1;              // alone on your spec: the peer is you in this round (your best tonight)
		std::set<int32_t> UsedTonight;      // skills you used (cast or hit with) in any round tonight on this spec
		Role MyRole = R_Damage;
		std::vector<std::string> Jobs;      // your main jobs on this spec and build, most important first (empty: none known)
		std::string Build;                  // "with Ventari", "in Wanderer's gear", "on a support build"; "" when the spec has one list
		int RoleMetric = kMetricDamage;
		std::map<int32_t, std::string> Names;
		std::string Name(int32_t aSkill) const;
		std::string PeerLabel() const;      // "Druid" or "healer"
		bool LeftIsYou() const { return MeRaw && MeRaw->Pov; } // Compare: the left player is you
		std::string LeftName() const { return LeftIsYou() ? "You" : You.Name; }
	};
	// aLeft / aRight: accounts to compare instead of you and your compared player (the Compare tab); "" = the defaults
	Ctx BuildCtx(const std::vector<FightPtr>& aFights, int aIndex, bool aAllRounds, const std::string& aLeft = "", const std::string& aRight = "");
	// A player's main jobs over these rounds (their spec, and the build the log shows: legends, gear, Chronomancer
	// build); aBuild gets how the build reads in a sentence ("with Ventari"), "" if the spec has one list
	std::vector<std::string> JobsFor(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec, std::string* aBuild = nullptr);
	// The same, kept until the rounds or the choices change: use this for anything drawn every frame
	const Ctx& CachedCtx(const std::vector<FightPtr>& aFights, int aIndex, bool aAllRounds, const std::string& aLeft = "", const std::string& aRight = "");

	// The rounds and account to look a player up by: for your own best round (alone on your spec), that round and you
	std::pair<std::vector<FightPtr>, std::string> Lookup(const Ctx& c, const Player& p);

	// ---- helpers ------------------------------------------------------------------------------------------------
	std::string Num(double aValue);
	std::string Lower(std::string aText); // a measure's name inside a sentence
	std::string Clock(const std::string& aStamp);
	std::string Duration(int64_t aMs);
	double PerS(const Player& aPlayer, double aValue);
	std::string Share(int aPart, int aWhole, int aMinimum = 1);
	ImVec4 ProfessionColor(uint32_t aProf);
	Player Sum(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec,
		std::map<int32_t, std::string>& aNames);
	double GroupGenOver(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec, int aBoon);
	Role RoleOf(const Player& p);
	int CastsCutShort(const Player& p);
	int64_t DeadMs(const Player& p);
	const SkillRow* Row(const Player& p, int32_t aSkill);
	// Share of a player's damage that landed within 2 s of one of our spikes (%), over several rounds; -1 without damage
	double SpikeShare(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec);
	// Share of a player's healing and barrier in an enemy spike or the 3 s after (%); -1 without Healing Stats data
	double HealInEnemySpikes(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec);
	// Casts of a skill from 3 s before an enemy spike to 1 s into it: (timed, all casts) over these rounds
	std::pair<int, int> CastsOnEnemySpikes(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec, int32_t aSkill);
	constexpr int32_t kTaleOfTheAugustQueen = 76971; // Troubadour elite: distortion for nearby allies
	// Rounds where the player's revive skill went off with an ally down in reach, and rounds played (30 s+ alive).
	// Per round because the squad starts a fight with every cooldown ready: one use a round is the job.
	std::pair<int, int> RoundsWithRevive(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec);

	// Why a skill gave you less (or more) than the compared player: a word for tables, a sentence for the detail,
	// and the two numbers that show it (casts, casts in the right window, per cast, or output)
	struct Why
	{
		std::string Word, Sentence, Unit;
		double You = 0, Them = 0;
		std::string YouText, ThemText;
		double Gap = 0;       // compared player's rate minus yours, in the metric's rate unit
		bool NoCasts = false; // neither player cast it: a boon, condition, trait, relic or sigil
	};
	// aYouName: the left player's name when it isn't you (Compare with two other players), "" = you
	Why Explain(const Player& you, const Player& vs, const std::string& aVsName, int aMetric, int32_t aSkill, bool aOneRound, const std::string& aYouName = "");

	// ---- drawing ---------------------------------------------------------------------------------------------------
	void SpecIcon(const Player& p);
	void SpecIconAt(ImDrawList* dl, ImVec2 aPos, float aSize, const Player& p); // the class icon at a position
	void SkillIcon(int32_t aSkill, const std::string& aName);
	// A skill's icon at a position, or a box with its first letter while it loads (or in the render harness)
	void IconAt(ImDrawList* dl, ImVec2 aPos, float aSize, int32_t aSkill, const std::string& aName, ImU32 aFrame = 0);
	// The game's icon for a CC type or a boon (outlined in red when aStruck: stripped or corrupted), else a lettered box
	void CcIconAt(ImDrawList* dl, ImVec2 aPos, float aSize, Analysis::CcKind aKind);
	void BoonIconAt(ImDrawList* dl, ImVec2 aPos, float aSize, int aBoon, bool aStruck);
	const char* SkillDescription(int32_t aSkill, const std::string& aName); // the GW2 API's text, "" if unknown
	const char* BoonDescription(int aBoon);                                  // what the boon does, in a few words
	void Headers(const std::vector<std::pair<const char*, const char*>>& aColumns);
	void Cell(const std::string& aText, const ImVec4* aColor = nullptr);
	void NumCell(const std::string& aText, const ImVec4* aColor = nullptr);
	void BarCell(double aValue, double aMax, ImU32 aColor, const std::string& aText, bool aOutline = false);
	void Rect(ImDrawList* aList, ImVec2 aPos, float aWidth, float aHeight, ImU32 aColor);
	void SmallText(ImDrawList* aList, ImVec2 aPos, ImU32 aColor, const std::string& aText);
	void Key(ImU32 aColor, const char* aLabel); // legend swatch + label, on one line
	void Mark(ImDrawList* dl, ImVec2 aCentre, float aRadius, int aShape, ImU32 aColor); // 0 square, 1 circle, 2 diamond, 3 up, 4 down
	void ShapeKey(int aShape, ImU32 aColor, const char* aLabel);
	void Answer(const std::string& aText);      // the line that states the answer
	// One lane of the fight: our spike bands, enemy spike bands (and the 4 s before, for aWindow ahead), cast ticks
	void Lane(const Fight& f, const std::vector<int32_t>& aTimes, ImU32 aTick, float aWidth, float aHeight, int aWindow);
	void TimeAxis(const Fight& f, float aX, float aWidth);
	// What a skill did for you and for the compared player: numbers, reason, when you each cast it
	void SkillDetail(const Ctx& c, int aMetric, int32_t aSkill, bool aInCompare = false);

	// ---- the tabs --------------------------------------------------------------------------------------------------
	void ReviewTab(const Ctx& c);
	void DeathsTab(const Ctx& c);
	void RoundTab(const Ctx& c);
	void TonightTab(const Ctx& c);
	// Parts of the old Stability tab: the subgroup time line (Round), the givers table (Squad)
	void StabilityTimeLine(const Ctx& c);
	void StabilityGivers(const Ctx& c);
	// The revive skills used this round against the revive order, and the order itself (Deaths)
	void RevivesTable(const Ctx& c);
	// Is this player a damage player (their spec's first job, else by what they did)?
	bool IsDamagePlayer(const std::vector<FightPtr>& aFights, const Player& p);
	// Downs of ours this round with whether they died: (player, down span, died)
	struct Down { const Player* P; Analysis::Span S; bool Died; };
	std::vector<Down> Downs(const Fight& f);
	bool HadBoonAt(const Player& p, int aBoon, int32_t aMs);
	// Why there is no "you" in this round: you took no part in it, or the log's recorder isn't in the squad
	std::string NoYou(const Fight& f);
	void CompareTab(const Ctx& c);
	void SkillTable(const Ctx& c, int aMetric, bool aInReview); // Compare's table, also Review's measure level
	void SquadTab(const Ctx& c);
	// What led to a down or death: a short cause line and its parts (Fight and Review use it)
	struct DeathCause
	{
		std::string Line;                 // every fact, one line
		std::string Short;                // the damage and what stands out, for a collapsed row
		std::vector<std::string> Tags;
		std::vector<std::pair<std::string, std::vector<std::string>>> Groups; // Damage, Control, Boons, Support, Position
		double Damage = 0;
		std::string TopSkill;
	};
	DeathCause CauseOf(const Fight& f, const Player& p, const Analysis::Span& s);
	// The small summary window's lines: your role's number and rank, the top fix
	std::vector<std::string> SummaryLines(const Ctx& c);
}
