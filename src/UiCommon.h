#pragma once
// Shared parts of the Fight Review window: colours, the per-frame context (round, scope, you, the compared
// player), metrics, drawing helpers (bars, lanes) and the skill detail that Review and Compare both open.
// Design: HANDOFF.md, "Redesign agreed 2026-09-24".

#include <array>
#include <cstdint>
#include <functional>
#include <map>
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

	enum Tab { T_Review, T_Compare, T_Squad, T_Fight, T_TabCount };
	enum Role { R_Heal, R_Stab, R_Damage };
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
		// Compare
		int         Metric = -1;        // -1 = from your role
		int32_t     CompareOpen = kNoSkill;
		// Squad
		int         ColumnSet = 0;
		bool        AllBoons = false;
		// Fight
		bool        FightTable = false;
	};
	State& S();

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
		Role MyRole = R_Damage;
		int RoleMetric = kMetricDamage;
		std::map<int32_t, std::string> Names;
		std::string Name(int32_t aSkill) const;
		std::string PeerLabel() const;      // "Druid" or "healer"
	};
	Ctx BuildCtx(const std::vector<FightPtr>& aFights, int aIndex, bool aAllRounds);

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

	// Why a skill gave you less (or more) than the compared player: a word for tables, a sentence for the detail,
	// and the two numbers that show it (casts, casts in the right window, per cast, or output)
	struct Why
	{
		std::string Word, Sentence, Unit;
		double You = 0, Them = 0;
		std::string YouText, ThemText;
		double Gap = 0;       // compared player's rate minus yours, in the metric's rate unit
	};
	Why Explain(const Player& you, const Player& vs, const std::string& aVsName, int aMetric, int32_t aSkill, bool aOneRound);

	// ---- drawing ---------------------------------------------------------------------------------------------------
	void SpecIcon(const Player& p);
	void SkillIcon(int32_t aSkill, const std::string& aName);
	void Headers(const std::vector<std::pair<const char*, const char*>>& aColumns);
	void Cell(const std::string& aText, const ImVec4* aColor = nullptr);
	void NumCell(const std::string& aText, const ImVec4* aColor = nullptr);
	void BarCell(double aValue, double aMax, ImU32 aColor, const std::string& aText, bool aOutline = false);
	void Rect(ImDrawList* aList, ImVec2 aPos, float aWidth, float aHeight, ImU32 aColor);
	void SmallText(ImDrawList* aList, ImVec2 aPos, ImU32 aColor, const std::string& aText);
	void Key(ImU32 aColor, const char* aLabel); // legend swatch + label, on one line
	void Answer(const std::string& aText);      // the line that states the answer
	// One lane of the fight: our spike bands, enemy spike bands (and the 4 s before, for aWindow ahead), cast ticks
	void Lane(const Fight& f, const std::vector<int32_t>& aTimes, ImU32 aTick, float aWidth, float aHeight, int aWindow);
	void TimeAxis(const Fight& f, float aX, float aWidth);
	// What a skill did for you and for the compared player: numbers, reason, when you each cast it
	void SkillDetail(const Ctx& c, int aMetric, int32_t aSkill);

	// ---- the tabs --------------------------------------------------------------------------------------------------
	void ReviewTab(const Ctx& c);
	void CompareTab(const Ctx& c);
	void SkillTable(const Ctx& c, int aMetric, bool aInReview); // Compare's table, also Review's measure level
	void SquadTab(const Ctx& c);
	void FightTab(const Ctx& c);
}
