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

	enum Tab { T_Summary, T_You, T_Deaths, T_Compare, T_Round, T_Squad, T_TabCount };
	// What the window's right side shows (the v10 design, the user's pick 2026-10-04: the debrief at the left answers the
	// six questions, a click opens the detail at the right). The old tabs are panes here; SwitchTo's tabs map onto them.
	enum Pane { P_Home, P_Down, P_Downs, P_Revives, P_Round, P_You, P_Night, P_Compare, P_Squad, P_Calls, P_Help, P_Count };
	// The small window's lines (the user, 2026-10-04: 250 x 250 at most; each player picks the lines they read)
	enum MiniLine { ML_You, ML_Rank, ML_Why, ML_Downs, ML_Enemy, ML_Best, ML_Calls, ML_Count };
	enum Role { R_Heal, R_Stab, R_Damage, R_Strip };
	// A common build of a spec, from an audit of the user's logs (src/SpecJobs.inc)
	// A row of a spec's main jobs (src/SpecJobs.inc): appended when When holds for the player ("" = always)
	struct SpecJobs { const char* Spec; const char* When; std::vector<const char*> Jobs; };
	constexpr int32_t kNoSkill = INT32_MIN;

	// ---- state shared by the tabs ----------------------------------------------------------------------------
	struct State
	{
		int         Selected = -1;      // round index, -1 = follow the latest
		// This round / Tonight, per tab (the v6 audit: one global switch and a Tonight tab meant two things)
		bool        YouTonight = false;     // You: the night (fixes that keep coming back) instead of this round
		bool        CompareTonight = false; // Compare and Squad: every round loaded, added up
		bool        SquadTonight = false;
		std::string VsAccount;          // "" = the best on your spec
		int         SwitchTo = -1;      // a tab to select next frame (mapped onto a View)
		// The window's right side: the pane shown, and the way back (Back pops it). A step keeps what decides the pane.
		int         Shown = P_Home;
		struct Step { int Shown = P_Home; int64_t SpikeOpen = -1; bool SpikeEnemy = false; std::string DeathKey; int DeathFilter = 0; int RoundView = 0; std::string DeathPlayer; };
		std::vector<Step> Back;
		bool        KeepPane = false;   // the small window opened the window at a pane: a round that came meanwhile doesn't reset it
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
		int         ColumnSet = -1;     // Squad's column set; -1 until picked: your role's
		bool        AllBoons = false;
		// Deaths: the revive order
		std::vector<std::string> ReviveOrder; // accounts in your revive order; empty = the usual order (saved)
		// Stability
		int         StabGroup = -1;     // subgroup on the time line, -1 = yours
		// Round
		int         RoundView = 0;      // 0: spikes, 1: stability over time
		std::vector<int32_t> Picked;    // skills marked on the round's time line and drawn in the spike breakdown
		std::vector<int32_t> PickedEnemy; // enemy skills marked on the round's time line (their rails under the graph)
		std::set<int32_t> RailOpen, EnemyRailOpen; // marked skills whose rail shows a row per player (per enemy)
		std::set<std::string> CallPartOpen;           // Summary's calls: the parts opened in a card's detail ("card/part")
		std::string CallOpen;                      // Summary, this round's calls: the card opened ("" none)
		int64_t     SpikeOpen = -1;     // the spike broken down (its time), -1 = the round
		bool        SpikeEnemy = false; // the spike broken down is theirs
		std::string SpikeStamp;         // the round it belongs to
		// Deaths
		std::string DeathKey;           // the down shown: round/account/ms, "" = the first that fits the filter
		int         DeathFilter = 0;    // 0 everyone, 1 you, 2 your subgroup, 3 revives, 4 one enemy spike (DeathSpike), 5 one player (DeathPlayer)
		std::string DeathPlayer;        // the player of filter 5, by account (a click in the squad grid: their downs, earlier and later)
		int64_t     DeathSpike = -1;    // an enemy spike opened from the Round tab: its downs first
	};
	State& S();
	// Open a pane at the right, remembering the current one for Back (a click in the debrief, the small window, a link)
	void Go(int aPane);
	// The snapshot's version this frame (Session::Snapshot::Version): per-round caches key on it, so a round re-read
	// (or a new night) never leaves them pointing into rounds that are gone
	uint64_t DataVersion();
	void SetDataVersion(uint64_t aVersion);
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
	// One round: healing, damage, cleanses and strips as the round's totals (what ArcDPS and Healing Stats show; the
	// user, 2026-09-27: per minute made a short round's numbers look off). Several rounds: per second or per minute.
	// Boons are always stacks or % uptime.
	bool Totals(const Player& p);
	const char* RateUnit(const Metric& m, const Player& p); // "" for a total
	double Rate(const Metric& m, const Player& p, double aValue);
	std::string WithUnit(const std::string& aNumber, const char* aUnit); // "4.8k /s", or "351k"
	double CountRate(const Player& p, double aCount); // one round: the count; several: per minute alive
	std::string ShownName(const std::string& aMeasure, bool aOneRound); // "Cleanses /min" -> "Cleanses" for one round
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
	// A spike's window: from 3 s before its peak (the build-up: downs there were counted outside it, the user's 20:18
	// on 25 Sept) to 4 s after. On 110 held-out rounds, 1 s before caught 73% of our downs in an enemy spike, 3 s 86%,
	// with a spike that led to a down 59% -> 66% of the time; 4 s before adds little (the rate there is near the
	// baseline). The same holds for enemy downs in our spikes (68% -> 78%).
	// A spike is a run of seconds now (Analysis Spikes, 2026-10-06): the window runs from 2.5 s before the run's start to
	// 3.5 s after its end, the same as before for a one-second spike.
	constexpr int64_t kSpikeBeforeMs = 3000, kSpikeAfterMs = 4000;
	// The spike (its peak, ms) a moment belongs to, ours or theirs: the nearest whose window holds it; -1 none
	int64_t SpikeOf(const Fight& f, bool aOurs, int64_t aMs);
	// A spike's run (start of its first second, end of its last), from its peak; a peak not found: the peak's second
	std::pair<int64_t, int64_t> SpikeSpan(const Fight& f, bool aOurs, int64_t aPeak);
	// A fixed window around a spike's peak (aBefore before it, aAfter after), stretched over the spike's whole run: the same
	// window for a one-second spike. aAtStart: both ends against the run's start instead (what came before a spike, or
	// early in it, shouldn't stretch over a 12 s one)
	std::pair<int64_t, int64_t> SpikeWindow(const Fight& f, bool aOurs, int64_t aPeak, int64_t aBefore, int64_t aAfter, bool aAtStart = false);
	// In the window (as above) of any spike of that side
	bool InSpike(const Fight& f, bool aOurs, int64_t aMs, int64_t aBefore, int64_t aAfter, bool aAtStart = false);
	// One use of invulnerability by one of ours: the gives of one player within 0.3 s (Tale of the August Queen puts
	// Distortion on the whole group at once). Skill: what they cast then, else the buff's name.
	struct InvulnUse { int32_t Ms = 0, Duration = 0; int By = -1; std::vector<int> To; std::string Skill; };
	std::vector<InvulnUse> InvulnUses(const Fight& f, int64_t aFrom, int64_t aTo);
	// This round's calls (the Summary): a card per key skill this squad brought, judged by its own rule (UiCalls.cpp).
	// Group: 0 in our spikes, 1 before theirs, 2 after downs and both ways. Kind: 0 good, 1 partly, 2 off.
	// A step of a chain of actions (ChainAt, under drawing): a skill, a CC type or a boon taken, and how it went
	enum ChainState { CS_Done, CS_OffTime, CS_Missed, CS_Skipped };
	struct ChainStep { int32_t Skill = 0; int Cc = -1, Boon = -1; std::string Name, Value, Tip; int State = CS_Done; };
	// One cast on a row's strip, when a row is a whole call (the wells: every well of it on one strip)
	struct CallMark { int32_t Off = 0; bool Good = true; std::string Who, Skill; };
	// A table card's cell (Tale: a row per use; stability: a row per enemy spike, a column per subgroup). Kind: -1 plain,
	// 0 good, 1 partly, 2 bad (the text says it too), 3 not counted (grey); Downs: allies downed, a mark after the text
	struct CallCell { std::string Text, Tip; int Kind = -1; bool Right = false; int Downs = 0; };
	struct CallRow { std::string Who, Skill; int32_t Ms = 0; std::string Ref, What; bool Good = true; int32_t Offset = 0;
		std::string Part, Section; const Player* By = nullptr; int32_t At = 0;
		std::vector<CallMark> Marks; int32_t WinFrom = -1000, WinTo = 2500; bool NoteBad = false;
		std::vector<ChainStep> Chain;               // the actions of this row in order, as icons (a Warrior's opener)
		std::vector<std::vector<ChainStep>> Cells;  // a grid row: a cell per column of the card (each Necromancer's two wells)
		int Downs = -1;                             // enemies downed after this call (a grid row), -1: none shown
		std::vector<CallCell> Table; };             // a table card's row, under the card's Head
	// Ref: timed against that moment (a strip, What's text after two spaces is a note beside it, red when NoteBad);
	// WinFrom, WinTo: the on-the-call window on the strip; Marks: several casts on one strip; Part, Section: headings in
	// the detail; By, At: the player and the call's moment (hover: their skills around it)
	struct CallCard { std::string Key, Name, Should, Line, Short, Verdict, Detail; int Group = 0, Kind = 1, Num = 0, Den = 0; int32_t Skill = 0; std::vector<CallRow> Rows;
		std::map<std::string, std::string> PartTips; // Detail: how its rows are judged (the head's hover); PartTips: each part's (its heading's hover)
		std::vector<std::string> Columns;             // a grid card's columns (the wells: a Necromancer each), the rows' Cells under them
		std::vector<std::string> Head, HeadTips; };   // a table card's headings and their hovers (3 to 5 words); the rows' Table under them
	const std::vector<CallCard>& RoundCalls(const Fight& f);
	// Was this player (Players index) invulnerable at that moment, and from whose use
	const Analysis::Fight::InvulnGive* InvulnOn(const Fight& f, int aPlayer, int32_t aMs);
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
	// A chain of actions as small icons with arrows between (the down's step cards made small; the user, 2026-10-05: a
	// series of actions as icons, names on hover, not as text). A step: a skill, a CC type or a boon taken; how it went
	// (done; done off time: a gold frame; missed: dimmed, struck through, a red frame; not expected then: dimmed, struck
	// through in grey); a short value after it; its name and words on hover.
	float ChainWidth(float aSize, const std::vector<ChainStep>& aSteps, bool aArrows = true);
	float ChainAt(ImDrawList* dl, ImVec2 aPos, float aSize, const std::vector<ChainStep>& aSteps, bool aArrows = true); // returns the width
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
	void TipWrapped(const std::string& aText);  // a tooltip of free text, wrapped at about 35 characters' width (never the screen's)
	// The enemy teams that fought us this round (WvW: red, blue, green; the user, 2026-10-05: two teams at once are told
	// apart), most players first: players, downed, killed and classes per team. One entry, Team -1, when the log doesn't say.
	struct EnemyTeam { int Team = -1; int Players = 0, Downed = 0, Killed = 0; std::map<std::string, int> Specs; };
	std::vector<EnemyTeam> EnemyTeams(const Fight& f);
	const char* TeamName(int aTeam); // Red, Blue, Green, or Unknown team
	// This round / Tonight at the right of a tab's first line; aTip explains Tonight
	void ScopeSwitch(const char* aId, bool& aTonight, const char* aTip);
	// One lane of the fight: our spike bands, enemy spike bands (and the 4 s before, for aWindow ahead), cast ticks
	void Lane(const Fight& f, const std::vector<int32_t>& aTimes, ImU32 aTick, float aWidth, float aHeight, int aWindow);
	void TimeAxis(const Fight& f, float aX, float aWidth);
	// What a skill did for you and for the compared player: numbers, reason, when you each cast it
	void SkillDetail(const Ctx& c, int aMetric, int32_t aSkill, bool aInCompare = false);

	// ---- the tabs --------------------------------------------------------------------------------------------------
	void ReviewTab(const Ctx& c);
	void DeathsTab(const Ctx& c, int aMode = 0); // aMode: DeathsMode (below)
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
	// Who got a player up from a down (not a death), from what the log shows, most direct first: an Illusion of Life
	// put on them as they got up, a plain revive running then, a revive skill completed near them in the 3 s before.
	// Healing on them while downed as they got up (a revive skill's pulse) counts too. "" when none (see Rallied).
	// aSkill: the skill (1066 for a plain revive), 0 when none.
	std::string RevivedBy(const Fight& f, const Down& d, int32_t* aSkill);
	// Got up with no reviver: a rally when an enemy died within 0.1 s of it (30 Sept: 143 of 283 get-ups near an
	// enemy's end came at its death, to the 50 ms)
	bool Rallied(const Fight& f, const Down& d);
	// A player's position nearest a moment (within 2 s), else null
	const Player::Point* NearestPos(const Player& p, int32_t aMs);
	// How a down ended in a few words: "revived by Tam Vey, Spirit of Nature", "rallied: ...", or unknown
	std::string GotUpHow(const Fight& f, const Down& d);
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
	// The debrief (the window's left side, the v10 design): the round with its strip and three reasons, you, the squad's
	// downs as the game's squad window, the enemy, the best this round, the calls; every part opens its detail at the
	// right and has more on hover
	void Debrief(const Ctx& c);
	// The small window: the result, the strip, then the lines picked (aLines: a bit per MiniLine), each with the same
	// hover as the debrief; a click opens the window at that pane. Returns the height drawn.
	float SmallWindow(const Ctx& c, float aWidth, unsigned aLines, bool aShadow, bool aStrip);
	// The calls, each a card that opens who and when (the Summary's calls, now a pane of its own)
	void CallsView(const Ctx& c);
	// How to read the debrief (the ? button): what each part shows
	void HelpView();
	// Your down that the debrief shows first (the one that killed you, else the last): its Deaths key, "" if none
	std::string YourDownKey(const Ctx& c);
	// A spike's hover lines, as the debrief's strip shows them (who went down, both sides' top three skills): the spike of
	// that side (aAlly) nearest aMs within 1.5 s, drawn into the tooltip that's open; false when there's none
	bool SpikeTipLines(const Ctx& c, int32_t aMs, bool aAlly);
	// Deaths in parts: both (the old tab), the list of downs, one down, the revive skills and order
	enum DeathsMode { DM_Both, DM_List, DM_Detail, DM_Revives };
	// Your role's number this round against your usual tonight and the best on your spec, and the first fix
	struct NextRoundFacts
	{
		bool Has = false, Known = false, Stab = false, YouBest = false, HasFix = false;
		std::string Word;                        // "CC on your subgroup covered", "healing /s"
		double You = 0, Usual = -1, Best = -1;   // -1: not known
		std::string YouText, UsualText, BestText, BestWho;
		std::string FixSubject, FixWhat, FixNumbers;
		int32_t FixSkill = kNoSkill;
	};
	NextRoundFacts NextRound(const Ctx& c);
	// Your lead measure this round (the You tab's first card): where you stand on your spec, for the debrief's rank line
	struct LeadFacts { bool Has = false, Leads = false, Unknown = false; std::string Name, YouText, BestText, BestWho, UsualText, Gap; int Rank = 1, Of = 1; };
	LeadFacts YourLead(const Ctx& c);
	// Our spikes this round and how many damage players landed theirs on time (the Round tab's rule), added up
	struct SpikeTally { int Spikes = 0, OnTime = 0, Alive = 0; };
	SpikeTally OurSpikeTally(const Ctx& c);
}
