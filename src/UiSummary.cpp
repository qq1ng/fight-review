// The debrief, the window's left side (the v10 design, at a 1153 px window): the six
// questions in fixed places, each part a click into its detail at the right and more on hover. The round (the result,
// both sides' damage with every spike on a strip, CC both ways against tonight), you (your down as steps, your rank on
// your spec), the squad's downs as the game's squad window (a row per subgroup), the enemy, the best this round, the
// calls. The small window shows the same in lines (250 px; the player picks the lines). The calls' cards are a pane of
// their own. Worked out once per round: the window draws every frame.
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "Causes.h"
#include "Ui.h"
#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		const ImU32 kMid = IM_COL32(0xc9, 0xa2, 0x27, 255);       // partly: gold (never alone: the diamond says it too)
		const ImU32 kSelBg = IM_COL32(0x1c, 0x22, 0x2c, 255);     // the part whose detail is open at the right
		const ImU32 kHoverBg = IM_COL32(0x22, 0x27, 0x30, 255);
		const ImU32 kRule = IM_COL32(0x2a, 0x2e, 0x36, 255);
		const ImU32 kStripBg = IM_COL32(0x0c, 0x0d, 0x10, 255);
		const ImVec4 kWorseV(0xd9 / 255.0f, 0x59 / 255.0f, 0x26 / 255.0f, 1.0f); // the enemy's orange, for text

		// ---- the facts, worked out once per round -----------------------------------------------------------------

		struct SkillSum { int32_t Skill = 0; std::string Name, Who; int64_t Damage = 0; int Hits = 0; };
		struct TopList { std::vector<SkillSum> Top; int64_t Total = 0, Rest = 0; int RestCount = 0; int Enemies = 0; };

		// Why a player had no stability at a moment: never had any, ran out, used up by an earlier CC, stripped or corrupted
		struct StabState
		{
			bool Had = false;
			std::string Word;    // "corrupted 0.4 s before"
			std::string Short;   // "stability corrupted", for a down's line
			std::string How;     // "stripped", "corrupted", "used up", "ran out", "never had any"
			int32_t LostMs = -1; // when it went
			std::string Detail;  // hover: by whom and when
			std::string Last;    // hover: who last gave them stability
		};

		// A spike on the strip (ours and theirs at the same moment share one): who went down, each side's top three
		struct Zone
		{
			int64_t Ms = 0, OurMs = -1, TheirMs = -1;
			int EnemyDowns = 0, Died = 0;
			std::vector<std::pair<std::string, bool>> Downs; // ours downed in their spike: name, died
			std::vector<SkillSum> OurTop, TheirTop;
			int64_t OurTotal = 0, TheirTotal = 0;
		};

		// One of the Round part's lines: this round against the middle of tonight's rounds. Kind: 0 better, 1 about, 2 worse
		struct Reason { std::string Text, Value, Unit, Usual; int Kind = 1; std::string Short; }; // Short: the small window's

		// A moment of your down, for the hovers: when (from the down), what, which skill, which enemy
		struct StepRow { int32_t Rel = 0; std::string What, Skill, Spec; int32_t SkillId = 0; };

		// The round's CC or strips on you by skill, when you weren't downed: Extra counts CC with no stability on, or
		// stability stripped
		struct SkillCount { int32_t Skill = 0; std::string Name, Spec; int Count = 0, Extra = 0; };

		struct DownInfo { int32_t Ms = 0; bool Died = false; std::string Why, End, Key; };

		// The best this round in one job, and what got them there
		struct Best
		{
			std::string Label, Value, Head, Note, Note2, SkillsTitle, Unit;
			const Player* P = nullptr;
			std::vector<SkillSum> Skills; // Damage: the value shown
			int Metric = -1;
		};

		struct Facts
		{
			// the round
			std::string Score, Result, Length, Players;
			ImU32 ResultCol = 0;
			bool Lost = false;
			int EnemyDowns = 0, EnemyDeaths = 0, SquadDowns = 0, SquadDeaths = 0;
			int64_t OurDamage = 0, TheirDamage = 0;
			std::vector<Zone> Zones;
			std::vector<Reason> Reasons;
			std::vector<std::string> ReasonTip;
			std::vector<const Player*> TopPlayers;
			std::vector<SkillSum> TheirTopSkills;
			// both sides' totals (the old Summary's tiles, now the result's hover): CC, strips, damage by skill
			int OurCc = 0, CcOnUs = 0, CcOnUsBare = 0, OurStrips = 0, TheirStrips = 0;
			TopList OurTop, TheirTop;
			SpikeTally Ours;
			NextRoundFacts Next;
			int InTheirSpikes = 0, StrippedFirst = 0, CcWin = 0, CcHit = 0;
			// you
			bool HasYou = false;
			std::string NoYouText;
			bool WentDown = false, Died = false, FirstSpike = false;
			std::string DeathKey, Reviver;
			int32_t DownMs = 0, EndMs = 0;
			int64_t DownSpike = -1;
			int DeadS = 0, Downs = 0;
			bool StabStep = false;
			std::string StabHow;
			int32_t StabRel = 0;
			std::vector<StepRow> StabRows, OtherRows, CcRows;
			bool CcStep = false;
			Analysis::CcKind CcKind = Analysis::CC_Other;
			int32_t CcRel = 0, CcDur = 0, CcSkill = 0;
			std::string CcSkillName, CcSpec, CcStabNote;
			double BurstDmg = 0;
			int32_t BurstLen = 0;
			TopList YourHits;
			std::string YourHitsTitle;
			int CcTaken = 0, CcBare = 0, StripsTaken = 0, StabStripped = 0, Corrupted = 0;
			int64_t DamageTaken = 0;
			std::vector<SkillCount> CcBy, StripBy;
			Analysis::CcKind CcMain = Analysis::CC_Stun;
			LeadFacts Lead;
			double AliveRate = -1; // your lead number over the time you were alive, when you were dead a while
			// downs, the squad as the game's squad window
			struct Row { int Sg = 0; bool Cmd = false; std::vector<const Player*> Members; };
			std::vector<Row> Grid;
			std::map<const Player*, std::vector<DownInfo>> DownsOf;
			struct SgFact { int Sg = 0, Downs = 0, Died = 0; bool Cmd = false; };
			std::vector<SgFact> Worst;
			int CoverSg = -1, CoverWin = 0, CoverHit = 0;
			// the enemy
			struct Spec { std::string Name; int Count = 0; Player Icon; };
			std::vector<Spec> Enemy;
			struct TeamComp { int Team = -1; int Count = 0; std::vector<Spec> Specs; };
			std::vector<TeamComp> Teams; // the enemy teams that fought us, most players first
			int EnemyTotal = 0;
			std::string CopyLine;
			int WorstZone = -1;
			std::string TopSpec;
			int TopSpecCount = 0;
			double TopSpecShare = 0;
			int64_t TopSpecDamage = 0, SpikeDamage = 0;
			std::vector<SkillSum> TopSpecSkills;
			// the best this round
			std::vector<Best> Bests;
		};

		std::string SkillName(const Fight& f, int32_t aSkill)
		{
			auto it = f.SkillNames.find(aSkill);
			return it == f.SkillNames.end() ? std::to_string(aSkill) : it->second;
		}

		std::string Plural(const std::string& aWord, int aCount) { return aCount == 1 ? aWord : aWord + "s"; }

		std::string Secs(int32_t aMs)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.1f s", aMs / 1000.0);
			return buf;
		}

		// "-0.44 s" from the down (the minus sign as the game's font has it)
		std::string Rel(int32_t aMs)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%+.2f s", aMs / 1000.0);
			return buf;
		}

		std::string One(double aValue)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.1f", aValue);
			return buf;
		}

		std::string Pct(double aValue) { return std::to_string(static_cast<int>(aValue + 0.5)) + "%"; }

		std::string Ordinal(int n)
		{
			static const char* kOrd[] = {"th", "st", "nd", "rd"};
			return std::to_string(n) + kOrd[(n % 10 < 4 && (n / 10) % 10 != 1) ? n % 10 : 0];
		}

		std::string EnemySpec(const Fight& f, int aEnemy) { return aEnemy >= 0 && aEnemy < static_cast<int>(f.Enemies.size()) ? f.Enemies[aEnemy].Spec : std::string("NPC or siege"); }

		// The enemy strike that came with a CC or strip (same enemy, within 50 ms, as Deaths does it): its skill, 0 if none
		int32_t StrikeWith(const Player& p, int32_t aMs, int aEnemy)
		{
			int32_t best = 0, gap = 51;
			for (const auto& h : p.HitsIn)
			{
				int32_t d = std::abs(h.Ms - aMs);
				if (d < gap && (aEnemy < 0 || h.Enemy == aEnemy)) { gap = d; best = h.Skill; }
			}
			return best;
		}

		// The skills behind a set of hits, most damage first: the top 10 and the rest summed
		TopList Top(const Fight& f, const std::vector<const Player::TakenHit*>& aHits, bool aEnemyWho)
		{
			TopList t;
			std::map<std::string, SkillSum> by;
			std::map<std::string, std::set<int>> who;
			std::set<int> enemies;
			for (const auto* h : aHits)
			{
				std::string name = SkillName(f, h->Skill);
				SkillSum& s = by[name];
				if (s.Hits == 0) { s.Skill = h->Skill; s.Name = name; }
				s.Damage += h->Damage;
				s.Hits++;
				t.Total += h->Damage;
				who[name].insert(h->Enemy);
				if (h->Enemy >= 0) { enemies.insert(h->Enemy); }
			}
			t.Enemies = static_cast<int>(enemies.size());
			for (auto& [name, s] : by)
			{
				if (aEnemyWho)
				{
					// the spec that used it, or how many enemies when several specs did
					std::set<std::string> specs;
					int players = 0;
					for (int e : who[name]) { if (e >= 0) { specs.insert(f.Enemies[e].Spec); players++; } }
					s.Who = specs.empty() ? "NPC or siege" : specs.size() == 1 ? (players > 1 ? std::to_string(players) + " " + Plural(*specs.begin(), players) : *specs.begin())
						: std::to_string(players) + " enemies";
				}
				t.Top.push_back(s);
			}
			std::sort(t.Top.begin(), t.Top.end(), [](const SkillSum& a, const SkillSum& b) { return a.Damage > b.Damage; });
			for (size_t i = 10; i < t.Top.size(); i++) { t.Rest += t.Top[i].Damage; t.RestCount++; }
			if (t.Top.size() > 10) { t.Top.resize(10); }
			return t;
		}

		// A player's down contribution by skill: their hits on enemies under 90% health who went down next, with no heal
		// back above 90% before it (Analysis' rule, as Elite Insights counts it), most first
		std::vector<SkillSum> DownContributionBySkill(const Fight& f, const Player& p, size_t aN)
		{
			std::map<int32_t, SkillSum> by;
			for (const auto& h : p.HitsOut)
			{
				if (h.Enemy < 0) { continue; }
				const auto& en = f.Enemies[h.Enemy];
				auto at = std::upper_bound(en.Hp.begin(), en.Hp.end(), h.Ms, [](int32_t ms, const std::pair<int32_t, int32_t>& u) { return ms < u.first; });
				if (at == en.Hp.begin() || std::prev(at)->second > 9000) { continue; }
				bool downed = false;
				int32_t nextDown = INT32_MAX;
				for (const auto& sp : en.DownSpans)
				{
					if (sp.Dead) { continue; }
					if (sp.From <= h.Ms && sp.To > h.Ms) { downed = true; break; }
					if (sp.From >= h.Ms) { nextDown = std::min(nextDown, sp.From); }
				}
				if (downed || nextDown == INT32_MAX) { continue; }
				auto above = std::find_if(at, en.Hp.end(), [&](const std::pair<int32_t, int32_t>& u) { return u.first > h.Ms && u.second > 9000; });
				if (above != en.Hp.end() && above->first < nextDown) { continue; }
				SkillSum& s = by[h.Skill];
				s.Skill = h.Skill;
				s.Damage += h.Damage;
				s.Hits++;
			}
			std::vector<SkillSum> out;
			for (auto& [sk, s] : by) { s.Name = SkillName(f, sk); out.push_back(s); }
			std::sort(out.begin(), out.end(), [](const SkillSum& a, const SkillSum& b) { return a.Damage > b.Damage; });
			if (out.size() > aN) { out.resize(aN); }
			return out;
		}

		StabState StabAt(const Fight& f, const Player& p, int32_t aMs)
		{
			StabState s;
			int idx = static_cast<int>(&p - f.Players.data());
			// who last gave them stability before this moment
			int32_t lastMs = -1;
			for (const Player& q : f.Players)
			{
				for (const auto& g : q.StabGives)
				{
					if (g.Ms > aMs || g.Ms <= lastMs || std::find(g.Targets.begin(), g.Targets.end(), idx) == g.Targets.end()) { continue; }
					lastMs = g.Ms;
					s.Last = "Last given: " + std::string(&q == &p ? "yourself" : q.Name) + (g.Skill ? ", " + SkillName(f, g.Skill) : std::string()) + ", " + Secs(aMs - g.Ms) + " before";
				}
			}
			if (HadBoonAt(p, Analysis::kStability, aMs - 50)) { s.Had = true; s.Word = "stability on"; return s; }
			int32_t end = -1;
			for (auto& [a, b] : p.BoonOn[Analysis::kStability]) { if (b <= aMs) { end = b; } }
			if (end < 0)
			{
				s.How = "never had any";
				s.Word = "never had any";
				s.Short = "no stability given";
				s.Detail = "No stability on you this round before it.";
				return s;
			}
			s.LostMs = end;
			std::string ago = Secs(aMs - end) + " before";
			for (const auto& st : p.StripsIn)
			{
				if (st.Boon != Analysis::kStability || std::abs(st.Ms - end) > 150) { continue; }
				s.How = st.Corrupted ? "corrupted" : "stripped";
				s.Word = s.How + " " + ago;
				s.Short = "stability " + s.How;
				s.Detail = "Stability " + s.How + " " + ago + (st.Enemy >= 0 ? " by a " + f.Enemies[st.Enemy].Spec : std::string()) + ".";
				return s;
			}
			for (auto& [ms, kind] : p.StabLost)
			{
				if (kind != 1 || std::abs(ms - end) > 150) { continue; }
				s.How = "used up";
				s.Word = "used up " + ago;
				s.Short = "stability used up";
				s.Detail = "An earlier CC took the last stack, " + ago + ".";
				return s;
			}
			s.How = "ran out";
			s.Word = "ran out " + ago;
			s.Short = "stability ran out";
			s.Detail = "Your last stability ran out " + ago + ".";
			return s;
		}

		// The most damage to health in 2 s before the down: (damage, length in ms)
		std::pair<double, int32_t> Burst(const Player& p, int32_t t)
		{
			double best = 0;
			int32_t len = 0;
			for (const auto& a : p.HitsIn)
			{
				if (a.Ms < t - 6000 || a.Ms > t) { continue; }
				double sum = 0;
				for (const auto& b : p.HitsIn) { if (b.Ms >= a.Ms && b.Ms <= a.Ms + 2000 && b.Ms <= t) { sum += b.Damage - b.Barrier; } }
				if (sum > best) { best = sum; len = std::max<int32_t>(100, std::min(t, a.Ms + 2000) - a.Ms); }
			}
			return {best, len};
		}

		const Player::CcHit* LastCc(const Player& p, int32_t aFrom, int32_t aTo)
		{
			const Player::CcHit* out = nullptr;
			for (const auto& h : p.CcIn) { if (h.Ms >= aFrom && h.Ms <= aTo) { out = &h; } }
			return out;
		}

		double Median(std::vector<double> v)
		{
			if (v.empty()) { return -1; }
			std::sort(v.begin(), v.end());
			return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
		}

		// CC both ways: a CC in the log is one that landed; one that stability blocked only used up a stack (in tonight's
		// logs, 2 or 3 of 150 to 240 CC on allies came with a stack still on). Blocked and landed, on allies and on enemies.
		// CC both ways, until allies stopped fighting (Causes::StopMs: the round was decided; the CC on a squad running for
		// the reset isn't what decided it)
		struct CcWays { int AllyBlocked = 0, AllyLanded = 0, EnemyBlocked = 0, EnemyLanded = 0; int32_t Until = -1; };
		CcWays CcOf(const Fight& g)
		{
			CcWays w;
			w.Until = Causes::StopMs(g.ToPlayersPerS);
			auto in = [&](int32_t aMs) { return w.Until < 0 || aMs < w.Until; };
			for (const Player& p : g.Players)
			{
				for (const auto& [ms, kind] : p.StabLost) { w.AllyBlocked += kind == 1 && in(ms); }
				for (int32_t ms : p.CcMs) { w.AllyLanded += in(ms); }
			}
			for (const auto& e : g.Enemies) { for (const auto& b : e.StabBlocked) { w.EnemyBlocked += in(b.Ms); } }
			for (int32_t ms : g.OurCcMs) { w.EnemyLanded += in(ms); }
			return w;
		}
		double Share(int aPart, int aOther) { return aPart + aOther > 0 ? 100.0 * aPart / (aPart + aOther) : -1; }

		void Cover(const Fight& g, int& aWindows, int& aCovered)
		{
			aWindows = aCovered = 0;
			for (auto& [sg, n] : g.GroupCcWindows)
			{
				aWindows += n;
				auto it = g.GroupCcCovered.find(sg);
				aCovered += it == g.GroupCcCovered.end() ? 0 : it->second;
			}
		}

		Facts Work(const Ctx& c)
		{
			const Fight& f = *c.F;
			Facts x;
			// the result
			x.Score = std::to_string(f.EnemyDowns) + " : " + std::to_string(f.SquadDowns);
			x.Result = f.EnemyDowns > f.SquadDowns ? "WON" : f.EnemyDowns < f.SquadDowns ? "LOST" : "EVEN";
			x.ResultCol = f.EnemyDowns > f.SquadDowns ? kYou : f.EnemyDowns < f.SquadDowns ? kEnemy : kPeer;
			x.Lost = f.EnemyDowns < f.SquadDowns;
			x.Length = Duration(f.DurationMs);
			x.Players = std::to_string(f.SquadCount) + " v " + std::to_string(f.EnemyCount);
			x.EnemyDowns = f.EnemyDowns; x.EnemyDeaths = f.EnemyDeaths; x.SquadDowns = f.SquadDowns; x.SquadDeaths = f.SquadDeaths;
			x.OurDamage = f.SquadDamage;
			x.TheirDamage = f.EnemyDamage;
			std::vector<Down> downs = Downs(f);

			// the strip's spikes: both sides', the two at one moment as one; who went down and each side's top three
			{
				std::vector<std::pair<int64_t, bool>> peaks;
				for (int64_t t : f.OurSpikesMs) { peaks.push_back({t, true}); }
				for (int64_t t : f.TheirSpikesMs) { peaks.push_back({t, false}); }
				std::sort(peaks.begin(), peaks.end());
				for (auto [t, ours] : peaks)
				{
					if (x.Zones.empty() || t - x.Zones.back().Ms > 1500) { x.Zones.push_back({}); x.Zones.back().Ms = t; }
					Zone& z = x.Zones.back();
					if (ours)
					{
						z.OurMs = t;
						for (int32_t d : f.EnemyDownMs) { z.EnemyDowns += SpikeOf(f, true, d) == t; }
					}
					else
					{
						z.TheirMs = t;
						for (const Down& d : downs) { if (SpikeOf(f, false, d.S.From) == t) { z.Downs.push_back({d.P->Name, d.Died}); z.Died += d.Died; } }
					}
				}
				for (Zone& z : x.Zones)
				{
					int64_t a = z.Ms - 3000, b = z.Ms + 3000;
					std::map<std::pair<int32_t, const Player*>, int64_t> us;
					std::vector<const Player::TakenHit*> them;
					for (const Player& p : f.Players)
					{
						for (const auto& h : p.HitsOut) { if (h.Ms >= a && h.Ms <= b) { us[{h.Skill, &p}] += h.Damage; z.OurTotal += h.Damage; } }
						for (const auto& h : p.HitsIn) { if (h.Ms >= a && h.Ms <= b) { them.push_back(&h); } }
					}
					std::vector<std::pair<int64_t, std::pair<int32_t, const Player*>>> order;
					for (auto& [k, v] : us) { order.push_back({v, k}); }
					std::sort(order.begin(), order.end(), [](auto& l, auto& r) { return l.first > r.first; });
					for (size_t i = 0; i < order.size() && i < 3; i++) { z.OurTop.push_back({order[i].second.first, SkillName(f, order[i].second.first), order[i].second.second->Name, order[i].first}); }
					TopList tl = Top(f, them, true);
					z.TheirTotal = tl.Total;
					for (size_t i = 0; i < tl.Top.size() && i < 3; i++) { z.TheirTop.push_back(tl.Top[i]); }
				}
			}

			// the round's CC both ways against the middle of tonight's rounds (3+ spikes each side). The spike lines (allies and
			// enemies downed per spike) were the result, not a reason, and led nearly every round: gone. Worst first in a lost
			// round, best first in a won one; the further from tonight's middle first when alike. Counted until allies stopped
			// fighting (CcOf).
			{
				std::vector<double> blocked, landed;
				for (const FightPtr& fp : *c.Fights)
				{
					const Fight& g = *fp;
					if (g.OurSpikesMs.size() < 3 || g.TheirSpikesMs.size() < 3) { continue; }
					CcWays cw = CcOf(g);
					if (double b = Share(cw.AllyBlocked, cw.AllyLanded); b >= 0) { blocked.push_back(b); }
					if (double l = Share(cw.EnemyLanded, cw.EnemyBlocked); l >= 0) { landed.push_back(l); }
				}
				for (int32_t d : f.SquadDownMs) { x.InTheirSpikes += SpikeOf(f, false, d) >= 0; }
				Cover(f, x.CcWin, x.CcHit);
				const double cv = x.CcWin ? 100.0 * x.CcHit / x.CcWin : -1;
				const CcWays cw = CcOf(f);
				const double bl = Share(cw.AllyBlocked, cw.AllyLanded), la = Share(cw.EnemyLanded, cw.EnemyBlocked);
				const double mb = blocked.size() >= 3 ? Median(blocked) : -1, ml = landed.size() >= 3 ? Median(landed) : -1;
				auto pctKind = [&](double aValue, double aUsual) { return aValue < 0 || aUsual < 0 ? 1 : aValue < aUsual - 3 ? 2 : aValue > aUsual + 3 ? 0 : 1; };
				// CC both ways: what stability blocked on allies, what of ours landed on enemies
				Reason rc{"CC on allies blocked", bl < 0 ? std::string("no CC") : Pct(bl), "", mb >= 0 ? "tonight " + Pct(mb) : std::string(), pctKind(bl, mb)};
				Reason rl{"Ally CC landed", la < 0 ? std::string("no CC") : Pct(la), "", ml >= 0 ? "tonight " + Pct(ml) : std::string(), pctKind(la, ml)};
				// the small window's line (250 px): the number in words, tonight's beside it when it fits
				rc.Short = "CC on allies blocked: " + rc.Value;
				rl.Short = "Ally CC landed: " + rl.Value;
				auto gap = [](double aValue, double aUsual) { return aValue < 0 || aUsual < 0 ? 0.0 : std::abs(aValue - aUsual); };
				const bool landedFirst = x.Lost ? (rl.Kind > rc.Kind || (rl.Kind == rc.Kind && gap(la, ml) > gap(bl, mb)))
					: (rl.Kind < rc.Kind || (rl.Kind == rc.Kind && gap(la, ml) > gap(bl, mb)));
				x.Reasons = landedFirst ? std::vector<Reason>{rl, rc} : std::vector<Reason>{rc, rl};
				auto range = [](const std::vector<double>& v)
				{
					if (v.empty()) { return std::string(); }
					auto [lo, hi] = std::minmax_element(v.begin(), v.end());
					return Pct(*lo) + " to " + Pct(*hi);
				};
				if (bl >= 0)
				{
					x.ReasonTip.push_back("CC on allies blocked " + Pct(bl) + ": stability took " + std::to_string(cw.AllyBlocked) + " of " + std::to_string(cw.AllyBlocked + cw.AllyLanded) +
						" enemy CC hits on allies; " + std::to_string(cw.AllyLanded) + " landed.");
					if (mb >= 0) { x.ReasonTip.push_back("   Tonight: " + Pct(mb) + " in the middle of " + std::to_string(blocked.size()) + " rounds (" + range(blocked) + ")."); }
				}
				if (la >= 0)
				{
					x.ReasonTip.push_back("Ally CC landed " + Pct(la) + ": " + std::to_string(cw.EnemyLanded) + " of " + std::to_string(cw.EnemyLanded + cw.EnemyBlocked) +
						" ally CC hits on enemies; their stability took " + std::to_string(cw.EnemyBlocked) + ".");
					if (ml >= 0) { x.ReasonTip.push_back("   Tonight: " + Pct(ml) + " (" + range(landed) + ")."); }
				}
				x.ReasonTip.push_back("   A CC hit: one CC on one player. A blocked one used up a stability stack.");
				if (cw.Until >= 0) { x.ReasonTip.push_back("   Counted until allies stopped fighting at " + Duration(cw.Until) + ": the CC after it didn't decide the round."); }
				if (x.CcWin > 0) { x.ReasonTip.push_back("   TopStats' CC coverage (a stack around the CC, 0.75 s): " + Pct(cv) + ", the Squad view's column."); }
			}

			// both sides' totals: CC (every CC hit, as TopStats counts it), strips, damage by skill and who used it
			{
				x.Ours = OurSpikeTally(c);
				x.OurCc = static_cast<int>(f.OurCcMs.size());
				x.OurStrips = static_cast<int>(f.OurStripsMs.size());
				std::vector<const Player::TakenHit*> allIn;
				for (const Player& p : f.Players)
				{
					x.TheirStrips += static_cast<int>(p.StripsIn.size());
					x.CcOnUs += p.CcTaken;
					x.CcOnUsBare += p.CcNoStab;
					for (const auto& h : p.HitsIn) { allIn.push_back(&h); }
				}
				x.TheirTop = Top(f, allIn, true);
				std::map<std::string, SkillSum> by;
				std::map<std::string, std::map<std::string, std::set<std::string>>> users; // name -> spec -> accounts
				for (const Player& p : f.Players)
				{
					for (auto& [sk, r] : p.Skills)
					{
						if (r.Damage <= 0) { continue; }
						std::string name = SkillName(f, sk);
						SkillSum& sum = by[name];
						if (sum.Damage == 0) { sum.Skill = sk; sum.Name = name; }
						sum.Damage += r.Damage;
						sum.Hits += r.Hits;
						users[name][p.Spec].insert(p.Account);
						x.OurTop.Total += r.Damage;
					}
				}
				for (auto& [name, sum] : by)
				{
					int n = 0;
					for (auto& [spec, who] : users[name]) { n += static_cast<int>(who.size()); }
					sum.Who = users[name].size() == 1 ? std::to_string(n) + " " + Plural(users[name].begin()->first, n) : std::to_string(n) + " players";
					x.OurTop.Top.push_back(sum);
				}
				std::sort(x.OurTop.Top.begin(), x.OurTop.Top.end(), [](const SkillSum& a, const SkillSum& b) { return a.Damage > b.Damage; });
				for (size_t i = 10; i < x.OurTop.Top.size(); i++) { x.OurTop.Rest += x.OurTop.Top[i].Damage; x.OurTop.RestCount++; }
				if (x.OurTop.Top.size() > 10) { x.OurTop.Top.resize(10); }
			}

			// the score's hover: our top three players, their top three skills in their spikes
			{
				std::vector<const Player*> ps;
				for (const Player& p : f.Players) { ps.push_back(&p); }
				std::sort(ps.begin(), ps.end(), [](const Player* a, const Player* b) { return a->Damage > b->Damage; });
				for (size_t i = 0; i < ps.size() && i < 3; i++) { if (ps[i]->Damage > 0) { x.TopPlayers.push_back(ps[i]); } }
				std::vector<const Player::TakenHit*> inSpikes;
				std::map<std::string, int64_t> bySpec;
				std::map<std::string, std::map<int32_t, int64_t>> specSkills;
				for (const Player& p : f.Players)
				{
					for (const auto& h : p.HitsIn)
					{
						bool in = false;
						in = InSpike(f, false, h.Ms, 3000, 3000);
						if (!in) { continue; }
						inSpikes.push_back(&h);
						std::string spec = EnemySpec(f, h.Enemy);
						bySpec[spec] += h.Damage;
						specSkills[spec][h.Skill] += h.Damage;
						x.SpikeDamage += h.Damage;
					}
				}
				TopList tl = Top(f, inSpikes, true);
				for (size_t i = 0; i < tl.Top.size() && i < 3; i++) { x.TheirTopSkills.push_back(tl.Top[i]); }
				// the class behind most of their spike damage
				for (auto& [spec, d] : bySpec) { if (d > x.TopSpecDamage && spec != "NPC or siege") { x.TopSpecDamage = d; x.TopSpec = spec; } }
				if (!x.TopSpec.empty())
				{
					x.TopSpecShare = x.SpikeDamage > 0 ? double(x.TopSpecDamage) / double(x.SpikeDamage) : 0;
					std::vector<std::pair<int64_t, int32_t>> order;
					for (auto& [sk, d] : specSkills[x.TopSpec]) { order.push_back({d, sk}); }
					std::sort(order.rbegin(), order.rend());
					for (size_t i = 0; i < order.size() && i < 5; i++) { x.TopSpecSkills.push_back({order[i].second, SkillName(f, order[i].second), x.TopSpec, order[i].first}); }
				}
			}

			// each of our downs: its cause in a few words and how it ended; downs that lost stability first
			for (const Down& d : downs)
			{
				DownInfo di;
				di.Ms = d.S.From;
				di.Died = d.Died;
				di.Why = CauseOf(f, *d.P, d.S).Short;
				di.End = d.Died ? std::string("died") : GotUpHow(f, d);
				di.Key = f.Stamp + "/" + d.P->Account + "/" + std::to_string(d.S.From);
				x.DownsOf[d.P].push_back(di);
				bool stripped = false;
				for (const auto& st : d.P->StripsIn) { stripped |= st.Boon == Analysis::kStability && st.Ms >= d.S.From - 6000 && st.Ms <= d.S.From; }
				x.StrippedFirst += stripped;
			}
			// the squad as the game's squad window: a row per subgroup, its members across
			{
				const int cmd = f.Commander >= 0 ? f.Players[f.Commander].Subgroup : -1;
				std::map<int, std::vector<const Player*>> bySg;
				for (const Player& p : f.Players) { bySg[p.Subgroup].push_back(&p); }
				std::vector<Facts::SgFact> sgs;
				for (auto& [g, list] : bySg)
				{
					std::sort(list.begin(), list.end(), [](const Player* a, const Player* b) { return a->Pov != b->Pov ? a->Pov : a->Name < b->Name; });
					x.Grid.push_back({g, g == cmd, list});
					Facts::SgFact s{g, 0, 0, g == cmd};
					for (const Player* p : list)
					{
						auto it = x.DownsOf.find(p);
						if (it == x.DownsOf.end()) { continue; }
						s.Downs += static_cast<int>(it->second.size());
						for (const DownInfo& di : it->second) { s.Died += di.Died; }
					}
					sgs.push_back(s);
				}
				std::sort(sgs.begin(), sgs.end(), [](const Facts::SgFact& a, const Facts::SgFact& b) { return a.Downs != b.Downs ? a.Downs > b.Downs : a.Died > b.Died; });
				for (size_t i = 0; i < sgs.size() && i < 2; i++) { if (sgs[i].Downs > 0) { x.Worst.push_back(sgs[i]); } }
				double worst = 2;
				for (auto& [g, n] : f.GroupCcWindows)
				{
					if (n < 4) { continue; }
					auto it = f.GroupCcCovered.find(g);
					int hit = it == f.GroupCcCovered.end() ? 0 : it->second;
					if (double(hit) / n < worst) { worst = double(hit) / n; x.CoverSg = g; x.CoverWin = n; x.CoverHit = hit; }
				}
			}

			// the enemy squad by spec, most first; their worst spike: the one that downed most of ours
			{
				std::map<std::string, int> specs;
				for (const auto& e : f.Enemies) { if (e.Fought) { specs[e.Spec]++; } }
				for (auto& [name, n] : specs) { x.Enemy.push_back({name, n, SpecPlayer(name)}); }
				std::sort(x.Enemy.begin(), x.Enemy.end(), [](const Facts::Spec& a, const Facts::Spec& b) { return a.Count != b.Count ? a.Count > b.Count : a.Name < b.Name; });
				for (auto& [name, n] : specs) { x.EnemyTotal += n; }
				x.TopSpecCount = specs.count(x.TopSpec) ? specs[x.TopSpec] : 0;
				// by team: two enemy teams at once are told apart
				for (const EnemyTeam& t : EnemyTeams(f))
				{
					Facts::TeamComp tc{t.Team, t.Players, {}};
					for (auto& [name, n] : t.Specs) { tc.Specs.push_back({name, n, SpecPlayer(name)}); }
					std::sort(tc.Specs.begin(), tc.Specs.end(), [](const Facts::Spec& a, const Facts::Spec& b) { return a.Count != b.Count ? a.Count > b.Count : a.Name < b.Name; });
					x.Teams.push_back(tc);
				}
				auto list = [](const std::vector<Facts::Spec>& aSpecs)
				{
					std::string out;
					for (size_t i = 0; i < aSpecs.size(); i++) { out += (i ? ", " : "") + std::to_string(aSpecs[i].Count) + " " + aSpecs[i].Name; }
					return out;
				};
				if (x.Teams.size() >= 2)
				{
					x.CopyLine = "Enemy " + std::to_string(x.EnemyTotal) + ":";
					for (size_t i = 0; i < x.Teams.size(); i++)
					{
						x.CopyLine += std::string(i ? "; " : " ") + TeamName(x.Teams[i].Team) + " " + std::to_string(x.Teams[i].Count) + " (" + list(x.Teams[i].Specs) + ")";
					}
				}
				else { x.CopyLine = "Enemy " + std::to_string(x.EnemyTotal) + ": " + list(x.Enemy); }
				for (size_t i = 0; i < x.Zones.size(); i++)
				{
					const Zone& z = x.Zones[i];
					if (z.TheirMs < 0) { continue; }
					if (x.WorstZone < 0) { x.WorstZone = static_cast<int>(i); continue; }
					const Zone& w = x.Zones[x.WorstZone];
					auto key = [](const Zone& q) { return std::make_tuple(q.Downs.size(), q.Died, q.TheirTotal); };
					if (key(z) > key(w)) { x.WorstZone = static_cast<int>(i); }
				}
			}

			// the best this round in each job, and what got them there
			{
				FightPtr one;
				for (const FightPtr& fp : *c.Fights) { if (fp.get() == c.F) { one = fp; } }
				std::vector<FightPtr> round;
				if (one) { round.push_back(one); }
				auto top = [&](std::function<double(const Player&)> aValue) -> const Player*
				{
					const Player* best = nullptr;
					double v = 0;
					for (const Player& p : f.Players) { double w = aValue(p); if (w > v) { v = w; best = &p; } }
					return best;
				};
				auto skills = [&](const Player& p, std::function<double(const SkillRow&)> aValue, size_t aN)
				{
					std::vector<SkillSum> out;
					for (auto& [sk, r] : p.Skills)
					{
						double v = aValue(r);
						if (v <= 0) { continue; }
						out.push_back({sk, sk == 0 ? std::string("no skill found (traits, relics, sigils)") : SkillName(f, sk), "", static_cast<int64_t>(v), r.Casts});
					}
					std::sort(out.begin(), out.end(), [](const SkillSum& a, const SkillSum& b) { return a.Damage > b.Damage; });
					if (out.size() > aN) { out.resize(aN); }
					return out;
				};
				auto nearOurs = [&](int32_t ms) { return InSpike(f, true, ms, 3000, 1000, true); };  // before our push or early in it
				auto nearTheirs = [&](int32_t ms) { return InSpike(f, false, ms, 3000, 3000); };
				if (const Player* p = top([](const Player& q) { return double(q.Damage); }))
				{
					Best b{"Damage", Num(double(p->Damage))};
					b.P = p;
					b.Head = "Most damage to players: " + Num(double(p->Damage));
					double share = round.empty() ? -1 : SpikeShare(round, p->Account, p->Spec);
					if (share >= 0) { b.Note = Pct(share) + " of it in ally spikes"; }
					b.Note2 = "also " + std::to_string(p->Strips) + " strips, " + std::to_string(p->CcDealt) + " CC on enemies";
					b.SkillsTitle = "Top skills";
					b.Skills = skills(*p, [](const SkillRow& r) { return double(r.Damage); }, 3);
					b.Metric = kMetricDamage;
					x.Bests.push_back(b);
				}
				if (const Player* p = top([](const Player& q) { return double(q.DownContribution); }))
				{
					Best b{"Down contr.", Num(double(p->DownContribution))};
					b.P = p;
					b.Head = "Most down contribution: " + Num(double(p->DownContribution)) + " on enemies who then went down";
					b.Note = "estimated, as Elite Insights counts it";
					int before = 0;
					for (int32_t ms : p->StripMs) { before += nearOurs(ms); }
					b.Note2 = "also " + std::to_string(p->Strips) + " strips, " + std::to_string(before) + " of them in the 3 s before ally spikes";
					b.SkillsTitle = "Top skills, by their down contribution";
					b.Skills = DownContributionBySkill(f, *p, 3);
					b.Metric = kMetricDamage;
					x.Bests.push_back(b);
				}
				if (const Player* p = top([](const Player& q) { return double(q.Strips); }))
				{
					Best b{"Strips", std::to_string(p->Strips)};
					b.P = p;
					int before = 0;
					for (int32_t ms : p->StripMs) { before += nearOurs(ms); }
					b.Head = "Most boons stripped: " + std::to_string(p->Strips);
					b.Note = std::to_string(before) + " in the 3 s before ally spikes";
					b.SkillsTitle = "By skill";
					b.Skills = skills(*p, [](const SkillRow& r) { return double(r.Strips); }, 4);
					b.Metric = kMetricStrips;
					x.Bests.push_back(b);
				}
				if (const Player* p = top([](const Player& q) { return double(q.Cleanses); }))
				{
					Best b{"Cleanses", std::to_string(p->Cleanses)};
					b.P = p;
					int in = 0;
					for (int32_t ms : p->CleanseMs) { in += nearTheirs(ms); }
					b.Head = "Most conditions cleansed: " + std::to_string(p->Cleanses);
					b.Note = std::to_string(in) + " in enemy spikes or the 3 s after";
					b.SkillsTitle = "By skill";
					b.Skills = skills(*p, [](const SkillRow& r) { return double(r.Cleanses); }, 4);
					b.Metric = kMetricCleanses;
					x.Bests.push_back(b);
				}
				if (const Player* p = top([&](const Player& q) { return f.GroupGeneration(q, Analysis::kStability); }))
				{
					char v[32];
					std::snprintf(v, sizeof(v), "%.2f", f.GroupGeneration(*p, Analysis::kStability));
					Best b{"Stability", v};
					b.P = p;
					b.Head = std::string("Most stability on their subgroup: ") + v + " stacks";
					if (p->StabSpikes > 0) { b.Note = "At " + std::to_string(p->StabSpikes) + " enemy spikes, " + Pct(100.0 * p->StabSpikeShare / p->StabSpikes) + " of sg " + std::to_string(p->Subgroup) + " had a stack from them in the 3 s before the peak"; }
					auto w = f.GroupCcWindows.find(p->Subgroup);
					auto h = f.GroupCcCovered.find(p->Subgroup);
					if (w != f.GroupCcWindows.end()) { b.Note2 = "CC on sg " + std::to_string(p->Subgroup) + ": " + std::to_string(h == f.GroupCcCovered.end() ? 0 : h->second) + " of " + std::to_string(w->second) + " covered by anyone's stability"; }
					b.SkillsTitle = "By skill (seconds of stability on the subgroup)";
					b.Unit = " s";
					b.Skills = skills(*p, [](const SkillRow& r) { return r.BoonGroupS[Analysis::kStability]; }, 4);
					b.Metric = kMetricGroupBoon + Analysis::kStability;
					x.Bests.push_back(b);
				}
				if (const Player* p = top([](const Player& q) { return q.HealKnown ? double(q.Heal) : 0.0; }))
				{
					Best b{"Healing", Num(double(p->Heal))};
					b.P = p;
					b.Head = "Most healing: " + Num(double(p->Heal)) + " in " + std::to_string(p->ActiveMs / 1000) + " s alive";
					for (const auto& sp : p->DownSpans) { if (sp.Dead) { b.Note = "died at " + Duration(sp.From); break; } }
					b.SkillsTitle = "Top skills";
					b.Skills = skills(*p, [](const SkillRow& r) { return double(r.Heal); }, 3);
					b.Metric = kMetricHeal;
					x.Bests.push_back(b);
				}
			}

			// you
			if (!c.MeRaw) { x.NoYouText = NoYou(f); return x; }
			x.HasYou = true;
			const Player& me = *c.MeRaw;
			x.Lead = YourLead(c);
			x.Next = NextRound(c);
			x.DeadS = static_cast<int>((f.DurationMs - me.ActiveMs) / 1000);
			std::vector<const Down*> mine;
			for (const Down& d : downs) { if (d.P == &me) { mine.push_back(&d); } }
			x.Downs = static_cast<int>(mine.size());
			if (mine.empty())
			{
				// not downed: the CC you took and whether stability was on, the damage you took
				x.CcTaken = static_cast<int>(me.CcIn.size());
				x.DamageTaken = me.DamageTaken;
				// the round's CC and strips on you, by skill (the enemy strike that came with it), most first
				// (no strike with it, as for a CC that does no damage: the skill is unknown, kept apart per enemy class)
				auto group = [&](std::map<std::pair<int32_t, std::string>, SkillCount>& aBy, int32_t aMs, int aEnemy, bool aExtra)
				{
					int32_t sk = StrikeWith(me, aMs, aEnemy);
					SkillCount& g = aBy[{sk, sk ? std::string() : EnemySpec(f, aEnemy)}];
					if (g.Count == 0) { g.Skill = sk; g.Name = sk ? SkillName(f, sk) : std::string("skill unknown"); g.Spec = EnemySpec(f, aEnemy); }
					else if (g.Spec != EnemySpec(f, aEnemy)) { g.Spec = "several classes"; }
					g.Count++;
					g.Extra += aExtra;
				};
				auto sorted = [](std::map<std::pair<int32_t, std::string>, SkillCount>& aBy)
				{
					std::vector<SkillCount> v;
					for (auto& [k, g] : aBy) { v.push_back(g); }
					std::stable_sort(v.begin(), v.end(), [](const SkillCount& a, const SkillCount& b) { return a.Count > b.Count; });
					if (v.size() > 8) { v.resize(8); }
					return v;
				};
				std::map<std::pair<int32_t, std::string>, SkillCount> cc, strips;
				std::map<int, int> kinds;
				for (const auto& h : me.CcIn)
				{
					bool bare = !StabAt(f, me, h.Ms).Had;
					x.CcBare += bare;
					group(cc, h.Ms, h.Enemy, bare);
					kinds[h.Kind]++;
				}
				for (const auto& st : me.StripsIn)
				{
					x.StripsTaken++;
					x.StabStripped += st.Boon == Analysis::kStability;
					x.Corrupted += st.Corrupted;
					group(strips, st.Ms, st.Enemy, st.Boon == Analysis::kStability);
				}
				x.CcBy = sorted(cc);
				x.StripBy = sorted(strips);
				int most = 0;
				for (auto& [k, n] : kinds) { if (n > most) { most = n; x.CcMain = static_cast<Analysis::CcKind>(k); } }
				std::vector<const Player::TakenHit*> hits;
				for (const auto& h : me.HitsIn) { hits.push_back(&h); }
				x.YourHits = Top(f, hits, true);
				x.YourHitsTitle = "Damage you took this round: " + Num(double(x.YourHits.Total));
				return x;
			}
			x.WentDown = true;
			// the down that killed you, else the last one
			const Down* d = mine.back();
			for (const Down* e : mine) { if (e->Died) { d = e; } }
			int32_t t = d->S.From;
			x.DownMs = t;
			x.EndMs = d->S.To;
			x.Died = d->Died;
			x.DeathKey = f.Stamp + "/" + me.Account + "/" + std::to_string(t);
			x.DownSpike = SpikeOf(f, false, t);
			x.FirstSpike = x.DownSpike >= 0 && x.DownSpike == f.TheirSpikesMs.front();
			if (!d->Died)
			{
				int32_t skill = 0;
				std::string by = RevivedBy(f, *d, &skill);
				x.Reviver = by.empty() ? GotUpHow(f, *d) : by + (skill > 0 && skill != 1066 ? ", " + SkillName(f, skill) : std::string());
			}
			const Player::CcHit* cc = LastCc(me, t - 6000, t);
			StabState st = StabAt(f, me, cc ? cc->Ms : t);
			// your stability: how it went, and every boon taken in the 6 s with the skill that took it
			if (!st.Had)
			{
				x.StabStep = true;
				x.StabHow = st.How;
				x.StabRel = st.LostMs >= 0 ? st.LostMs - t : 0;
			}
			for (const auto& s2 : me.StripsIn)
			{
				if (s2.Ms < t - 6000 || s2.Ms > t) { continue; }
				int32_t sk = StrikeWith(me, s2.Ms, s2.Enemy);
				StepRow r{s2.Ms - t, std::string(Analysis::kBoonNames[s2.Boon]) + (s2.Corrupted ? " corrupted" : " stripped"), sk ? SkillName(f, sk) : std::string("skill unknown"), EnemySpec(f, s2.Enemy), sk};
				(s2.Boon == Analysis::kStability ? x.StabRows : x.OtherRows).push_back(r);
			}
			// the last CC before the down, and every CC in the 6 s
			if (cc)
			{
				x.CcStep = true;
				x.CcKind = cc->Kind;
				x.CcRel = cc->Ms - t;
				x.CcDur = cc->Duration;
				x.CcSkill = StrikeWith(me, cc->Ms, cc->Enemy);
				x.CcSkillName = x.CcSkill ? SkillName(f, x.CcSkill) : std::string("skill unknown");
				x.CcSpec = EnemySpec(f, cc->Enemy);
				x.CcStabNote = st.Had ? "stability on, it landed anyway" : st.How == "never had any" ? "no stability: none given before it" : "no stability then: " + st.Word;
			}
			for (const auto& h : me.CcIn)
			{
				if (h.Ms < t - 6000 || h.Ms > t) { continue; }
				int32_t sk = StrikeWith(me, h.Ms, h.Enemy);
				x.CcRows.push_back({h.Ms - t, std::string(Analysis::kCcVerbs[h.Kind]) + " " + Secs(h.Duration), sk ? SkillName(f, sk) : std::string("skill unknown"), EnemySpec(f, h.Enemy), sk});
			}
			auto [burst, len] = Burst(me, t);
			x.BurstDmg = burst;
			x.BurstLen = len;
			std::vector<const Player::TakenHit*> hits;
			for (const auto& h : me.HitsIn) { if (h.Ms >= t - 6000 && h.Ms <= t) { hits.push_back(&h); } }
			x.YourHits = Top(f, hits, true);
			x.YourHitsTitle = Num(double(x.YourHits.Total)) + " from " + std::to_string(x.YourHits.Enemies) + Plural(" enemy", x.YourHits.Enemies).replace(0, 0, "") + " in the 6 s before your down";
			return x;
		}

		// Worked out once per round; the window and the small window can show different rounds, so a few are kept
		const Facts& FactsOf(const Ctx& c)
		{
			static std::map<std::string, std::pair<int, Facts>> cache; // key -> (last use, facts)
			static int clock = 0;
			std::string k = c.F->Stamp + "|" + std::to_string(DataVersion()) + "|" + (c.MeRaw ? c.MeRaw->Account : std::string()) + "|" + S().VsAccount + "|" +
				std::to_string(c.Fights ? c.Fights->size() : 0);
			auto it = cache.find(k);
			if (it == cache.end())
			{
				if (cache.size() >= 4)
				{
					auto oldest = cache.begin();
					for (auto e = cache.begin(); e != cache.end(); ++e) { if (e->second.first < oldest->second.first) { oldest = e; } }
					cache.erase(oldest);
				}
				it = cache.emplace(k, std::make_pair(0, Work(c))).first;
			}
			it->second.first = ++clock;
			return it->second.second;
		}

		// ---- drawing -----------------------------------------------------------------------------------------------

		float SmallSize() { return ImGui::GetFontSize() * 0.85f; }

		// Text with a dark copy under it, readable over the game when the small window has no background
		void Txt(ImDrawList* dl, ImVec2 p, ImU32 aCol, const std::string& aText, bool aShadow, float aSize = 0)
		{
			float size = aSize > 0 ? aSize : ImGui::GetFontSize();
			if (aShadow) { dl->AddText(ImGui::GetFont(), size, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 230), aText.c_str()); }
			dl->AddText(ImGui::GetFont(), size, p, aCol, aText.c_str());
		}

		float TextW(const std::string& aText, float aSize = 0)
		{
			return ImGui::GetFont()->CalcTextSizeA(aSize > 0 ? aSize : ImGui::GetFontSize(), FLT_MAX, 0, aText.c_str()).x;
		}

		// A down's cross (the font has no such glyph)
		void Cross(ImDrawList* dl, ImVec2 c, float r, ImU32 aCol)
		{
			dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), aCol, 2.0f);
			dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), aCol, 2.0f);
		}

		// The verdict's shape: circle better, diamond about the same, down triangle worse (never colour alone)
		void Verdict(ImDrawList* dl, ImVec2 c, float r, int aKind)
		{
			Mark(dl, c, r, aKind == 0 ? 1 : aKind == 1 ? 2 : 4, aKind == 0 ? kYou : aKind == 1 ? kMid : kEnemy);
		}

		// ---- the hovers --------------------------------------------------------------------------------------------

		void SkillRows(const Fight& f, const std::vector<SkillSum>& aRows, bool aWho, const std::string& aUnit = "")
		{
			if (aRows.empty()) { return; }
			if (!ImGui::BeginTable("skills", aWho ? 4 : 3, ImGuiTableFlags_SizingFixedFit)) { return; }
			for (const SkillSum& s : aRows)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImVec2 p = ImGui::GetCursorScreenPos();
				IconAt(ImGui::GetWindowDrawList(), p, ImGui::GetTextLineHeight(), s.Skill, s.Name);
				ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(s.Name.c_str());
				if (aWho) { ImGui::TableSetColumnIndex(2); ImGui::TextColored(kMuted, "%s", s.Who.c_str()); }
				NumCell(aUnit.empty() ? Num(double(s.Damage)) : std::to_string(s.Damage) + aUnit);
			}
			ImGui::EndTable();
			(void)f;
		}

		void StepTable(const std::vector<StepRow>& aRows)
		{
			if (aRows.empty() || !ImGui::BeginTable("steps", 4, ImGuiTableFlags_SizingFixedFit)) { return; }
			for (const StepRow& r : aRows)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0); ImGui::TextColored(kMuted, "%s", Rel(r.Rel).c_str());
				ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.What.c_str());
				ImGui::TableSetColumnIndex(2);
				ImVec2 p = ImGui::GetCursorScreenPos();
				if (r.SkillId) { IconAt(ImGui::GetWindowDrawList(), p, ImGui::GetTextLineHeight(), r.SkillId, r.Skill); ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight(), 1)); ImGui::SameLine(0, 4); }
				ImGui::TextUnformatted(r.Skill.c_str());
				ImGui::TableSetColumnIndex(3); ImGui::TextColored(kMuted, "%s", r.Spec.c_str());
			}
			ImGui::EndTable();
		}

		// A spike on the strip: the one the mouse is on first (aAlly: the ally spike), then the other side's at that moment.
		// The lines only (the Round view's graph puts them in its own tooltip); TipZone wraps them in one.
		void ZoneLines(const Fight& f, const Zone& z, bool aAlly, bool aStrip)
		{
			const bool both = z.OurMs >= 0 && z.TheirMs >= 0;
			auto enemyHead = [&]()
			{
				if (z.TheirMs < 0) { return; }
				std::string died = z.Died ? ", " + std::to_string(z.Died) + " died" : std::string();
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kEnemy), "Enemy spike %s: %d %s downed%s", Duration(z.TheirMs).c_str(), static_cast<int>(z.Downs.size()),
					z.Downs.size() == 1 ? "ally" : "allies", died.c_str());
			};
			auto allyHead = [&]()
			{
				if (z.OurMs < 0) { return; }
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kYou), "Ally spike %s: %d %s downed", Duration(z.OurMs).c_str(), z.EnemyDowns, z.EnemyDowns == 1 ? "enemy" : "enemies");
			};
			if (aAlly) { allyHead(); enemyHead(); } else { enemyHead(); allyHead(); }
			if (!z.Downs.empty())
			{
				std::string names;
				for (auto& [n, died] : z.Downs) { names += (names.empty() ? "" : ", ") + std::string(died ? "x " : "") + n; }
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
				ImGui::TextColored(kMuted, "Allies downed (x died): %s", names.c_str());
				ImGui::PopTextWrapPos();
			}
			auto enemy = [&](bool aFirst)
			{
				ImGui::TextColored(kMuted, aFirst ? "Enemy top three, %s in the 6 s around it" : "Enemy top three, %s", Num(double(z.TheirTotal)).c_str());
				ImGui::PushID("enemy");
				SkillRows(f, z.TheirTop, true);
				ImGui::PopID();
			};
			auto ally = [&](bool aFirst)
			{
				ImGui::TextColored(kMuted, aFirst ? "Ally top three, %s in the 6 s around it" : "Ally top three, %s", Num(double(z.OurTotal)).c_str());
				ImGui::PushID("ally");
				SkillRows(f, z.OurTop, true);
				ImGui::PopID();
			};
			if (aAlly) { ally(true); enemy(false); } else { enemy(true); ally(false); }
			ImGui::TextColored(kMuted, aAlly ? "Click: the ally spike broken down" : "Click: the enemy spike broken down");
			if (both)
			{
				ImGui::TextColored(kMuted, aAlly ? (aStrip ? "The enemy spike at this moment: the strip's lower half" : "The enemy spike at this moment: the graph's lower half")
					: (aStrip ? "The ally spike at this moment: the strip's upper half" : "The ally spike at this moment: the graph's upper half"));
			}
		}

		void TipZone(const Fight& f, const Zone& z, bool aAlly)
		{
			ImGui::BeginTooltip();
			ZoneLines(f, z, aAlly, true);
			ImGui::EndTooltip();
		}

		// Both sides' damage by skill, top 10 each, side by side in one table (the old Summary's damage tiles)
		void TopTables(const TopList& aOurs, const TopList& aTheirs)
		{
			if (!ImGui::BeginTable("tops", 11, ImGuiTableFlags_SizingFixedFit)) { return; }
			const float lh = ImGui::GetTextLineHeight();
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, lh);
			for (int i = 0; i < 4; i++) { ImGui::TableSetupColumn(""); }
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, lh * 0.8f); // the gap between the sides
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, lh);
			for (int i = 0; i < 4; i++) { ImGui::TableSetupColumn(""); }
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(1); ImGui::TextColored(kMuted, "Ally skills");
			ImGui::TableSetColumnIndex(2); ImGui::TextColored(kMuted, "Used by");
			NumCell("Hits", &kMuted);
			NumCell("Damage", &kMuted);
			ImGui::TableSetColumnIndex(7); ImGui::TextColored(kMuted, "Enemy skills on allies");
			ImGui::TableSetColumnIndex(8); ImGui::TextColored(kMuted, "Enemy");
			NumCell("Hits", &kMuted);
			NumCell("Damage", &kMuted);
			size_t rows = std::max(aOurs.Top.size(), aTheirs.Top.size());
			auto side = [&](const TopList& t, size_t i, int aCol)
			{
				if (i >= t.Top.size()) { return; }
				const SkillSum& r = t.Top[i];
				ImGui::TableSetColumnIndex(aCol);
				IconAt(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), lh, r.Skill, r.Name);
				ImGui::Dummy(ImVec2(lh, lh));
				ImGui::TableSetColumnIndex(aCol + 1); ImGui::TextUnformatted(r.Name.c_str());
				ImGui::TableSetColumnIndex(aCol + 2); ImGui::TextColored(kMuted, "%s", r.Who.c_str());
				NumCell(std::to_string(r.Hits));
				NumCell(Num(double(r.Damage)));
			};
			for (size_t i = 0; i < rows; i++)
			{
				ImGui::TableNextRow();
				side(aOurs, i, 0);
				side(aTheirs, i, 6);
			}
			ImGui::TableNextRow();
			auto rest = [](const TopList& t, int aCol)
			{
				if (t.RestCount == 0) { return; }
				ImGui::TableSetColumnIndex(aCol);
				ImGui::TextColored(kMuted, "and %s from %d more", Num(double(t.Rest)).c_str(), t.RestCount);
			};
			rest(aOurs, 1);
			rest(aTheirs, 7);
			ImGui::EndTable();
		}

		// The result: both sides' numbers, our top players, both sides' damage by skill
		void TipScore(const Facts& x, const Fight& f)
		{
			ImGui::BeginTooltip();
			if (ImGui::BeginTable("sides", 3, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableNextRow();
				NumCell("Ally", &kMuted);
				NumCell("Enemy", &kMuted);
				auto line = [](const char* aWhat, const std::string& aUs, const std::string& aThem)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(aWhat);
					NumCell(aUs);
					NumCell(aThem);
				};
				ImGui::TableSetColumnIndex(0);
				line("Downed", std::to_string(x.EnemyDowns), std::to_string(x.SquadDowns));
				line("Died", std::to_string(x.EnemyDeaths), std::to_string(x.SquadDeaths));
				line("Spikes", std::to_string(f.OurSpikesMs.size()), std::to_string(f.TheirSpikesMs.size()));
				line("CC", std::to_string(x.OurCc), std::to_string(x.CcOnUs));
				line("Boons stripped", std::to_string(x.OurStrips), std::to_string(x.TheirStrips));
				line("Damage", Num(double(x.OurDamage)), Num(double(x.TheirDamage)));
				ImGui::EndTable();
			}
			ImGui::TextColored(kMuted, "Downed: players each side downed. CC on allies: %d with no stability just before.", x.CcOnUsBare);
			ImGui::Spacing();
			ImGui::TextColored(kMuted, "Ally top three players");
			for (const Player* p : x.TopPlayers)
			{
				SpecIcon(*p);
				ImGui::Text("%s  %s", p->Name.c_str(), Num(double(p->Damage)).c_str());
			}
			ImGui::Spacing();
			ImGui::TextColored(kMuted, "Damage by skill to players, the top 10 each side: allies %s, enemies %s (pets and minions not counted)", Num(double(x.OurTop.Total)).c_str(), Num(double(x.TheirTop.Total)).c_str());
			TopTables(x.OurTop, x.TheirTop);
			ImGui::TextColored(kMuted, "Click: the round, with marked skills on its time line");
			ImGui::EndTooltip();
			(void)f;
		}

		void TipReasons(const Facts& x)
		{
			ImGui::BeginTooltip();
			for (const std::string& line : x.ReasonTip)
			{
				if (line.rfind("   ", 0) == 0 || line.rfind("A spike", 0) == 0) { ImGui::TextColored(kMuted, "%s", line.c_str() + (line[0] == ' ' ? 3 : 0)); }
				else { ImGui::TextUnformatted(line.c_str()); }
			}
			if (x.Ours.Alive > 0)
			{
				ImGui::Text("Damage players on time in ally spikes: %d of %d", x.Ours.OnTime, x.Ours.Alive);
				ImGui::TextColored(kMuted, "On time: the player's hits landed with the spike, as the round's spike list counts it.");
			}
			ImGui::TextColored(kMuted, "Click: the round, with every spike in order");
			ImGui::EndTooltip();
		}

		void TipDown(const Facts& x, const Fight& f)
		{
			ImGui::BeginTooltip();
			if (x.Died) { ImGui::Text("Down at %s, died %s later", Duration(x.DownMs).c_str(), Secs(x.EndMs - x.DownMs).c_str()); }
			else { ImGui::Text("Down at %s, up %s later: %s", Duration(x.DownMs).c_str(), Secs(x.EndMs - x.DownMs).c_str(), x.Reviver.c_str()); }
			if (x.DownSpike >= 0) { ImGui::TextColored(kMuted, "In the enemy spike at %s%s", Duration(x.DownSpike).c_str(), x.FirstSpike ? ", the first" : ""); }
			else { ImGui::TextColored(kMuted, "Outside enemy spikes"); }
			if (x.Downs > 1) { ImGui::TextColored(kMuted, "%d downs this round; this is %s", x.Downs, x.Died ? "the one you died in" : "the last"); }
			if (x.DeadS > 0) { ImGui::TextColored(kMuted, "Dead %d of %d s", x.DeadS, static_cast<int>(f.DurationMs / 1000)); }
			ImGui::TextColored(kMuted, "Click: your down, step by step");
			ImGui::EndTooltip();
		}

		void TipStab(const Facts& x)
		{
			ImGui::BeginTooltip();
			ImGui::Text("Your stability: %s%s", x.StabHow.c_str(), x.StabRel ? (" at " + Rel(x.StabRel)).c_str() : "");
			if (!x.StabRows.empty()) { ImGui::TextColored(kMuted, "Taken from you in the 6 s before the down"); ImGui::PushID("stab"); StepTable(x.StabRows); ImGui::PopID(); }
			if (!x.OtherRows.empty()) { ImGui::TextColored(kMuted, "Other boons (%d)", static_cast<int>(x.OtherRows.size())); ImGui::PushID("other"); StepTable(x.OtherRows); ImGui::PopID(); }
			ImGui::EndTooltip();
		}

		void TipCc(const Facts& x)
		{
			ImGui::BeginTooltip();
			ImGui::Text("%s %s by %s (%s), %s before the down", Analysis::kCcVerbs[x.CcKind], Secs(x.CcDur).c_str(), x.CcSkillName.c_str(), x.CcSpec.c_str(), Secs(-x.CcRel).c_str());
			ImGui::TextColored(kMuted, "%s", x.CcStabNote.c_str());
			ImGui::TextColored(kMuted, "CC on you in the 6 s (%d)", static_cast<int>(x.CcRows.size()));
			StepTable(x.CcRows);
			ImGui::EndTooltip();
		}

		void TipHits(const Facts& x, const Fight& f)
		{
			ImGui::BeginTooltip();
			if (x.WentDown) { ImGui::Text("%s to health in %s; %s", Num(x.BurstDmg).c_str(), Secs(x.BurstLen).c_str(), x.YourHitsTitle.c_str()); }
			else { ImGui::TextUnformatted(x.YourHitsTitle.c_str()); }
			SkillRows(f, x.YourHits.Top, true);
			if (x.YourHits.RestCount > 0) { ImGui::TextColored(kMuted, "and %s from %d more %s", Num(double(x.YourHits.Rest)).c_str(), x.YourHits.RestCount, x.YourHits.RestCount == 1 ? "skill" : "skills"); }
			ImGui::EndTooltip();
		}

		// The round's CC or strips on you by skill, most first (you weren't downed)
		void TipCounts(const Fight& f, const std::string& aHead, const std::vector<SkillCount>& aRows, const char* aExtra)
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(aHead.c_str());
			if (!aRows.empty() && ImGui::BeginTable("counts", 5, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(1); ImGui::TextColored(kMuted, "Skill");
				ImGui::TableSetColumnIndex(2); ImGui::TextColored(kMuted, "Enemy");
				NumCell("Times", &kMuted);
				NumCell(aExtra, &kMuted);
				for (const SkillCount& r : aRows)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					IconAt(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), ImGui::GetTextLineHeight(), r.Skill, r.Name);
					ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
					ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.Name.c_str());
					ImGui::TableSetColumnIndex(2); ImGui::TextColored(kMuted, "%s", r.Spec.c_str());
					NumCell(std::to_string(r.Count));
					NumCell(std::to_string(r.Extra));
				}
				ImGui::EndTable();
			}
			(void)f;
			ImGui::TextColored(kMuted, "Click: you against your spec, skill by skill");
			ImGui::EndTooltip();
		}

		void TipRank(const Facts& x, const Ctx& c)
		{
			ImGui::BeginTooltip();
			const LeadFacts& l = x.Lead;
			ImGui::Text("%s: %s", l.Name.c_str(), l.YouText.c_str());
			if (!l.BestText.empty()) { ImGui::Text("%s %s (%s)", l.Leads ? "next" : "best", l.BestText.c_str(), l.BestWho.c_str()); }
			if (!l.UsualText.empty()) { ImGui::TextColored(kMuted, "your usual tonight: %s", l.UsualText.c_str()); }
			if (x.DeadS >= 20 && c.F) { ImGui::TextColored(kMuted, "You were dead %d of %d s", x.DeadS, static_cast<int>(c.F->DurationMs / 1000)); }
			if (x.Next.HasFix)
			{
				// the first fix, as the You pane lists them
				ImGui::Spacing();
				ImGui::Text("Fix first: %s, %s", x.Next.FixSubject.c_str(), x.Next.FixWhat.c_str());
				if (!x.Next.FixNumbers.empty()) { ImGui::TextColored(kMuted, "%s", x.Next.FixNumbers.c_str()); }
			}
			ImGui::TextColored(kMuted, x.Next.HasFix ? "Click: that fix open, skill by skill" : "Click: you against the best on your spec, skill by skill");
			ImGui::EndTooltip();
		}

		void TipPlayer(const Facts& x, const Player& p)
		{
			ImGui::BeginTooltip();
			SpecIcon(p);
			ImGui::Text("%s%s", p.Name.c_str(), p.Pov ? " (you)" : "");
			ImGui::SameLine();
			ImGui::TextColored(kMuted, "%s, subgroup %d", p.Spec.c_str(), p.Subgroup);
			auto it = x.DownsOf.find(&p);
			if (it == x.DownsOf.end()) { ImGui::TextColored(kMuted, "Not downed"); }
			else
			{
				for (const DownInfo& d : it->second)
				{
					ImVec2 m = ImGui::GetCursorScreenPos();
					float lh = ImGui::GetTextLineHeight();
					if (d.Died) { Cross(ImGui::GetWindowDrawList(), ImVec2(m.x + lh * 0.5f, m.y + lh * 0.5f), lh * 0.25f, kEnemy); }
					else { Mark(ImGui::GetWindowDrawList(), ImVec2(m.x + lh * 0.5f, m.y + lh * 0.5f), lh * 0.3f, 4, kEnemy); }
					ImGui::Dummy(ImVec2(lh, lh));
					ImGui::SameLine(0, 4);
					ImGui::Text("%s  %s", Duration(d.Ms).c_str(), d.Why.c_str());
					ImGui::SameLine();
					if (d.Died) { ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kEnemy), "died"); }
					else { ImGui::TextColored(kMuted, "%s", d.End.c_str()); }
				}
				ImGui::TextColored(kMuted, "Click: that down, step by step");
			}
			ImGui::EndTooltip();
		}

		// A subgroup's downs in time order: when, who, what downed them, how it ended
		void TipSg(const Facts& x, int aSg)
		{
			std::vector<std::pair<const DownInfo*, const Player*>> rows;
			for (const auto& row : x.Grid)
			{
				if (row.Sg != aSg) { continue; }
				for (const Player* p : row.Members)
				{
					auto it = x.DownsOf.find(p);
					if (it == x.DownsOf.end()) { continue; }
					for (const DownInfo& d : it->second) { rows.push_back({&d, p}); }
				}
			}
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first->Ms < b.first->Ms; });
			ImGui::BeginTooltip();
			ImGui::Text("Subgroup %d: %d %s", aSg, static_cast<int>(rows.size()), rows.size() == 1 ? "down" : "downs");
			if (!rows.empty() && ImGui::BeginTable("sgdowns", 4, ImGuiTableFlags_SizingFixedFit))
			{
				for (auto& [d, p] : rows)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::TextColored(kMuted, "%s", Duration(d->Ms).c_str());
					ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(p->Name.c_str());
					ImGui::TableSetColumnIndex(2); ImGui::TextColored(kMuted, "%s", d->Why.c_str());
					ImGui::TableSetColumnIndex(3);
					if (d->Died) { ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kEnemy), "died"); } else { ImGui::TextColored(kMuted, "got up"); }
				}
				ImGui::EndTable();
			}
			ImGui::TextColored(kMuted, "Click: every down, in time order");
			ImGui::EndTooltip();
		}

		void TipBest(const Fight& f, const Best& b)
		{
			ImGui::BeginTooltip();
			SpecIcon(*b.P);
			ImGui::Text("%s", b.P->Name.c_str());
			ImGui::SameLine();
			ImGui::TextColored(kMuted, "%s, subgroup %d", b.P->Spec.c_str(), b.P->Subgroup);
			ImGui::TextUnformatted(b.Head.c_str());
			if (!b.Note.empty()) { ImGui::TextColored(kMuted, "%s", b.Note.c_str()); }
			if (!b.Note2.empty()) { ImGui::TextColored(kMuted, "%s", b.Note2.c_str()); }
			if (!b.Skills.empty()) { ImGui::TextColored(kMuted, "%s", b.SkillsTitle.c_str()); SkillRows(f, b.Skills, false, b.Unit); }
			ImGui::TextColored(kMuted, "Click: compare them with the next best on their spec");
			ImGui::EndTooltip();
		}

		void TipCall(const CallCard& k)
		{
			ImGui::BeginTooltip();
			ImGui::Text("%s: %s", k.Name.c_str(), k.Verdict.c_str());
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
			ImGui::TextUnformatted(k.Line.c_str());
			std::set<std::string> off;
			if (k.Key == "warrior" || k.Key == "chrono" || k.Key == "necro" || k.Key == "ele" || k.Key == "tale" || k.Key == "spinal")
			{
				for (const CallRow& r : k.Rows) { if (!r.Good && r.Who != "nobody") { off.insert(r.Who); } }
			}
			if (!off.empty())
			{
				std::string who;
				for (const std::string& n : off) { who += (who.empty() ? "" : ", ") + n; }
				ImGui::TextColored(kMuted, "off: %s", who.c_str());
			}
			ImGui::PopTextWrapPos();
			ImGui::TextColored(kMuted, "Click: who and when, by spike");
			ImGui::EndTooltip();
		}

		// ---- pieces both the debrief and the small window draw -------------------------------------------------------

		// Open a spike at the right: the round's pane with that spike broken down (the ally one or the enemy one; a moment
		// where both sides spiked has both)
		void OpenSpike(const Fight& f, const Zone& z, bool aAlly)
		{
			State& s = S();
			Go(P_Round);
			s.RoundView = 0;
			const bool ally = aAlly ? z.OurMs >= 0 : z.TheirMs < 0;
			s.SpikeOpen = ally ? z.OurMs : z.TheirMs;
			s.SpikeEnemy = !ally;
			s.SpikeStamp = f.Stamp;
		}

		// The round strip: ally damage to enemy players per second above the line, enemy damage below, spikes shaded in
		// their side's half. Downs in a spike sit in lanes of their own, never over the bars: a triangle up and the count
		// above (enemies downed in an ally spike), a triangle down and the count below (allies downed in an enemy spike).
		// You: x died, a white triangle downed. Hover: the spike under the mouse, the top half the ally one and the bottom
		// half the enemy one; a click opens it.
		void Strip(const Facts& x, const Fight& f, const Player* aMe, ImVec2 p, float w, float h, bool aShadow, bool aSmall)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImGui::SetCursorScreenPos(p);
			ImGui::PushID("strip");
			bool clicked = ImGui::InvisibleButton("##strip", ImVec2(w, h));
			bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();
			const double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
			auto X = [&](double ms) { return p.x + static_cast<float>(std::clamp(ms / span, 0.0, 1.0)) * w; };
			const float ss = SmallSize(), lane = ss + 2;
			const float top = p.y + lane, bottom = p.y + h - lane, mid = (top + bottom) * 0.5f;
			if (!aShadow) { dl->AddRectFilled(ImVec2(p.x, top), ImVec2(p.x + w, bottom), kStripBg); }
			for (int64_t t : f.OurSpikesMs) { auto [a, b] = SpikeWindow(f, true, t, 600, 600); dl->AddRectFilled(ImVec2(X(double(a)), top), ImVec2(X(double(b)), mid), IM_COL32(57, 135, 229, 55)); }
			for (int64_t t : f.TheirSpikesMs) { auto [a, b] = SpikeWindow(f, false, t, 600, 600); dl->AddRectFilled(ImVec2(X(double(a)), mid), ImVec2(X(double(b)), bottom), IM_COL32(217, 89, 38, 60)); }
			double mx = 1;
			for (int64_t v : f.ToPlayersPerS) { mx = std::max(mx, double(v)); }
			for (int64_t v : f.InPerS) { mx = std::max(mx, double(v)); }
			size_t n = std::max(f.ToPlayersPerS.size(), f.InPerS.size());
			float bw = std::max(1.0f, w / std::max<size_t>(1, n) * 0.8f), half = mid - top - 1;
			for (size_t sec = 0; sec < n; sec++)
			{
				float bx = X(sec * 1000.0);
				if (sec < f.ToPlayersPerS.size() && f.ToPlayersPerS[sec] > 0) { float bh = static_cast<float>(f.ToPlayersPerS[sec] / mx) * half; dl->AddRectFilled(ImVec2(bx, mid - bh), ImVec2(bx + bw, mid), kYou); }
				if (sec < f.InPerS.size() && f.InPerS[sec] > 0) { float bh = static_cast<float>(f.InPerS[sec] / mx) * half; dl->AddRectFilled(ImVec2(bx, mid + 1), ImVec2(bx + bw, mid + 1 + bh), kEnemy); }
			}
			dl->AddLine(ImVec2(p.x, mid), ImVec2(p.x + w, mid), IM_COL32(0x3a, 0x3e, 0x45, 255));
			// the lanes: a triangle at the spike and its count after it; where two would touch, the later one waits for the hover
			float upEnd = -FLT_MAX, downEnd = -FLT_MAX;
			const float r = ss * 0.3f;
			for (const Zone& z : x.Zones)
			{
				if (z.OurMs >= 0 && z.EnemyDowns > 0)
				{
					float cx = X(double(z.OurMs));
					std::string t = std::to_string(z.EnemyDowns);
					if (cx - r >= upEnd + 2)
					{
						Mark(dl, ImVec2(cx, p.y + lane * 0.5f), r, 3, kYou);
						Txt(dl, ImVec2(cx + r + 1, p.y), IM_COL32(0x8c, 0xbc, 0xf7, 255), t, aShadow, ss);
						upEnd = cx + r + 1 + TextW(t, ss);
					}
				}
				if (z.TheirMs >= 0 && !z.Downs.empty())
				{
					float cx = X(double(z.TheirMs));
					std::string t = std::to_string(z.Downs.size());
					if (cx - r >= downEnd + 2)
					{
						Mark(dl, ImVec2(cx, bottom + lane * 0.5f), r, 4, kEnemy);
						Txt(dl, ImVec2(cx + r + 1, bottom + 1), IM_COL32(0xf3, 0xa0, 0x7c, 255), t, aShadow, ss);
						downEnd = cx + r + 1 + TextW(t, ss);
					}
				}
			}
			if (aMe)
			{
				for (const auto& sp : aMe->DownSpans)
				{
					if (sp.Dead) { continue; }
					bool died = false;
					for (const auto& q : aMe->DownSpans) { died |= q.Dead && std::abs(q.From - sp.To) < 100; }
					float cx = X(double(sp.From)), cy = (mid + bottom) * 0.5f, rr = (bottom - mid) * 0.32f;
					if (died) { Cross(dl, ImVec2(cx, cy), rr * 0.8f, ImGui::GetColorU32(ImGuiCol_Text)); }
					else { Mark(dl, ImVec2(cx, cy), rr, 4, ImGui::GetColorU32(ImGuiCol_Text)); }
				}
			}
			// the spike under the mouse: the top half looks for an ally spike, the bottom half for an enemy one
			if (hovered && !x.Zones.empty())
			{
				const ImVec2 m = ImGui::GetIO().MousePos;
				const bool upper = m.y < mid;
				double at = (m.x - p.x) / w * span;
				int best = -1;
				for (int pass = 0; pass < 2 && best < 0; pass++)
				{
					double gap = 2500;
					for (size_t i = 0; i < x.Zones.size(); i++)
					{
						const Zone& z = x.Zones[i];
						if (pass == 0 && (upper ? z.OurMs < 0 : z.TheirMs < 0)) { continue; }
						double d = std::abs(double(z.Ms) - at);
						if (d < gap) { gap = d; best = static_cast<int>(i); }
					}
				}
				if (best >= 0)
				{
					const Zone& z = x.Zones[best];
					const bool ally = upper ? z.OurMs >= 0 : z.TheirMs < 0;
					dl->AddRect(ImVec2(X(z.Ms - 1200.0), ally ? top : mid), ImVec2(X(z.Ms + 1200.0), ally ? mid : bottom), ImGui::GetColorU32(ImGuiCol_Text));
					TipZone(f, z, ally);
					if (clicked)
					{
						// from the small window: open the window on the latest round at that spike
						if (aSmall) { ShowWindow = true; S().Selected = -1; S().KeepPane = true; }
						OpenSpike(f, z, ally);
					}
				}
			}
		}

		// The verdict word and the score, big: "LOST 8 : 30" (the result also says it in a word, never colour alone)
		float ScoreLine(const Facts& x, ImVec2 p, bool aShadow, float aScale)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float size = ImGui::GetFontSize() * aScale;
			Txt(dl, p, x.ResultCol, x.Result, aShadow, size);
			float cx = p.x + TextW(x.Result, size) + size * 0.4f;
			std::string ours = std::to_string(x.EnemyDowns), theirs = std::to_string(x.SquadDowns);
			Txt(dl, ImVec2(cx, p.y), kYou, ours, aShadow, size);
			cx += TextW(ours, size);
			Txt(dl, ImVec2(cx, p.y), ImGui::GetColorU32(kMuted), " : ", aShadow, size);
			cx += TextW(" : ", size);
			Txt(dl, ImVec2(cx, p.y), kEnemy, theirs, aShadow, size);
			return cx + TextW(theirs, size);
		}

		// The lines both draw: you, your rank, the first reason, downs, the enemy, the best, the calls
		struct LineCtx { const Facts* X; const Ctx* C; ImDrawList* Dl; float W; bool Shadow; };

		void YouLine(const LineCtx& L, ImVec2 p, bool aDetail = true)
		{
			const Facts& x = *L.X;
			float lh = ImGui::GetTextLineHeight();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			if (!x.HasYou) { Txt(L.Dl, p, muted, x.NoYouText, L.Shadow); return; }
			float cx = p.x + lh * 0.5f;
			std::string head, sub;
			if (x.WentDown && x.Died)
			{
				Cross(L.Dl, ImVec2(cx, p.y + lh * 0.5f), lh * 0.24f, kEnemy);
				head = "Died " + Duration(x.EndMs);
				sub = "down " + Duration(x.DownMs) + (x.DownSpike >= 0 ? (x.FirstSpike ? ", first enemy spike" : ", enemy spike") : "") + (x.DeadS >= 10 ? " \xc2\xb7 dead " + std::to_string(x.DeadS) + " s" : "");
			}
			else if (x.WentDown)
			{
				Mark(L.Dl, ImVec2(cx, p.y + lh * 0.5f), lh * 0.3f, 4, kEnemy);
				head = "Downed " + Duration(x.DownMs);
				sub = "up after " + Secs(x.EndMs - x.DownMs) + (x.Reviver.empty() ? "" : " \xc2\xb7 " + x.Reviver);
			}
			else
			{
				Mark(L.Dl, ImVec2(cx, p.y + lh * 0.5f), lh * 0.3f, 1, kYou);
				head = "Not downed";
				// (the debrief has these on a line of their own, each with its hover)
				if (aDetail) { sub = "CC'd " + std::to_string(x.CcTaken) + ", " + std::to_string(x.CcBare) + " no stab \xc2\xb7 " + Num(double(x.DamageTaken)) + " taken"; }
			}
			float tx = p.x + lh * 1.2f;
			Txt(L.Dl, ImVec2(tx, p.y), ink, head, L.Shadow);
			tx += TextW(head) + lh * 0.5f;
			L.Dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + L.W, p.y + lh), true);
			Txt(L.Dl, ImVec2(tx, p.y), muted, sub, L.Shadow);
			L.Dl->PopClipRect();
		}

		// The rank line's two links at its right end: where "compare >" starts, and "why >"
		float CompareLinkX(float aRight) { return aRight - TextW("compare >") - ImGui::GetTextLineHeight() * 0.6f - TextW("why >"); }
		float WhyLinkX(float aRight) { return aRight - TextW("why >"); }

		void RankLine(const LineCtx& L, ImVec2 p, bool aWhy, int aHot = 0) // aHot: 1 "compare >" hovered, 2 "why >"
		{
			const Facts& x = *L.X;
			float lh = ImGui::GetTextLineHeight();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			const LeadFacts& l = x.Lead;
			if (!l.Has) { Txt(L.Dl, p, muted, "Nobody to compare with on your spec", L.Shadow); return; }
			int kind = l.Unknown || l.Of <= 1 ? 1 : l.Leads ? 0 : l.Rank * 2 <= l.Of ? 1 : 2;
			Verdict(L.Dl, ImVec2(p.x + lh * 0.5f, p.y + lh * 0.5f), lh * 0.3f, kind);
			std::string head = l.Unknown ? "rank unknown" : l.Of > 1 ? Ordinal(l.Rank) + " of " + std::to_string(l.Of) : "alone on your spec";
			std::string sub = l.Name + " " + l.YouText + (l.BestText.empty() ? "" : " \xc2\xb7 " + std::string(l.Leads ? "next " : "best ") + l.BestText);
			float tx = p.x + lh * 1.2f;
			Txt(L.Dl, ImVec2(tx, p.y), ink, head, L.Shadow);
			tx += TextW(head) + lh * 0.5f;
			float end = aWhy ? CompareLinkX(p.x + L.W) : p.x + L.W;
			L.Dl->PushClipRect(ImVec2(tx, p.y), ImVec2(end - lh * 0.6f, p.y + lh), true);
			Txt(L.Dl, ImVec2(tx, p.y), muted, sub, L.Shadow);
			L.Dl->PopClipRect();
			if (aWhy)
			{
				Txt(L.Dl, ImVec2(end, p.y), aHot == 1 ? ink : kYou, "compare >", L.Shadow);
				Txt(L.Dl, ImVec2(WhyLinkX(p.x + L.W), p.y), aHot == 2 ? ink : kYou, "why >", L.Shadow);
			}
		}

		void ReasonLine(const LineCtx& L, ImVec2 p, const Reason& r, bool aCompact)
		{
			float lh = ImGui::GetTextLineHeight();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			Verdict(L.Dl, ImVec2(p.x + lh * 0.5f, p.y + lh * 0.5f), lh * 0.28f, r.Kind);
			float tx = p.x + lh * 1.2f;
			if (aCompact)
			{
				// "Their spikes: 2.1 downs each", then tonight's when it fits
				L.Dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + L.W, p.y + lh), true);
				Txt(L.Dl, ImVec2(tx, p.y), ink, r.Short, L.Shadow);
				if (!r.Usual.empty() && tx + TextW(r.Short) + lh * 0.5f + TextW(r.Usual) <= p.x + L.W)
				{
					Txt(L.Dl, ImVec2(p.x + L.W - TextW(r.Usual), p.y), muted, r.Usual, L.Shadow);
				}
				L.Dl->PopClipRect();
				return;
			}
			Txt(L.Dl, ImVec2(tx, p.y), ink, r.Text, L.Shadow);
			float vx = p.x + L.W * 0.62f;
			Txt(L.Dl, ImVec2(vx - TextW(r.Value), p.y), ink, r.Value, L.Shadow);
			Txt(L.Dl, ImVec2(vx + lh * 0.3f, p.y), muted, r.Unit, L.Shadow);
			if (!r.Usual.empty()) { Txt(L.Dl, ImVec2(p.x + L.W - TextW(r.Usual), p.y), muted, r.Usual, L.Shadow); }
		}

		// ---- the debrief's parts -------------------------------------------------------------------------------------

		struct Part { float Y0 = 0; bool Selected = false; };
	}

	std::string YourDownKey(const Ctx& c) { return FactsOf(c).DeathKey; }

	bool SpikeTipLines(const Ctx& c, int32_t aMs, bool aAlly)
	{
		const Facts& x = FactsOf(c);
		const Zone* best = nullptr;
		int64_t gap = 1500;
		for (const Zone& z : x.Zones)
		{
			int64_t t = aAlly ? z.OurMs : z.TheirMs;
			if (t >= 0 && std::llabs(t - aMs) <= gap) { gap = std::llabs(t - aMs); best = &z; }
		}
		if (!best) { return false; }
		ZoneLines(*c.F, *best, aAlly, false);
		return true;
	}

	void Debrief(const Ctx& c)
	{
		const Fight& f = *c.F;
		const Facts& x = FactsOf(c);
		State& s = S();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float lh = ImGui::GetTextLineHeight(), pad = lh * 0.55f, gap = lh * 0.25f;
		const ImVec2 o = ImGui::GetCursorScreenPos();
		const float W = ImGui::GetContentRegionAvail().x;
		const float x0 = o.x + pad, w = W - 2 * pad;
		const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
		float y = o.y;
		LineCtx L{&x, &c, dl, w, false};
		// a part's background goes under it once its height is known: content on channel 1, backgrounds on 0
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		auto label = [&](const std::string& aText, const std::string& aRight)
		{
			Txt(dl, ImVec2(x0, y), muted, aText, false, SmallSize());
			if (!aRight.empty()) { Txt(dl, ImVec2(x0 + w - TextW(aRight, SmallSize()), y), muted, aRight, false, SmallSize()); }
			y += lh;
		};
		auto row = [&](const char* aId, float aH, bool* aHovered) -> bool
		{
			ImGui::SetCursorScreenPos(ImVec2(x0 - 2, y));
			bool clicked = ImGui::InvisibleButton(aId, ImVec2(w + 4, aH));
			*aHovered = ImGui::IsItemHovered();
			if (*aHovered)
			{
				dl->ChannelsSetCurrent(0);
				dl->AddRectFilled(ImVec2(x0 - 2, y), ImVec2(x0 + w + 2, y + aH), kHoverBg);
				dl->ChannelsSetCurrent(1);
			}
			return clicked;
		};
		auto close = [&](const Part& aPart)
		{
			y += pad;
			if (aPart.Selected)
			{
				dl->ChannelsSetCurrent(0);
				dl->AddRectFilled(ImVec2(o.x, aPart.Y0), ImVec2(o.x + W, y), kSelBg);
				dl->ChannelsSetCurrent(1);
			}
			dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + W, y), kRule);
			y += 1;
		};
		bool hov = false;

		// ---- the round: the result, the strip, CC both ways -------------------------------------------------------------
		Part round{y, s.Shown == P_Round && s.SpikeOpen < 0};
		y += pad;
		label("ROUND", x.Length + " \xc2\xb7 " + x.Players);
		const float scale = 1.5f;
		if (row("score", lh * scale, &hov)) { Go(P_Round); s.SpikeOpen = -1; s.RoundView = 0; }
		if (hov) { TipScore(x, f); }
		float ex = ScoreLine(x, ImVec2(x0, y), false, scale);
		Txt(dl, ImVec2(ex + lh * 0.6f, y + lh * (scale - 1)), muted, "downed: enemies : allies", false, SmallSize());
		y += lh * scale + gap;
		{
			const float sh = lh * 2.4f + 2 * (SmallSize() + 2);
			Strip(x, f, c.MeRaw, ImVec2(x0, y), w, sh, false, false);
			y += sh + gap;
		}
		if (row("reasons", (lh + 2) * x.Reasons.size(), &hov)) { Go(P_Round); s.SpikeOpen = -1; s.RoundView = 0; }
		if (hov) { TipReasons(x); }
		for (const Reason& r : x.Reasons) { ReasonLine(L, ImVec2(x0, y), r, false); y += lh + 2; }
		close(round);

		// ---- why it was lost or won: the two factors furthest from how won fights look -----
		{
			std::vector<StruggleWhy> why;
			const int res = StruggleTop(c, why);
			if (!why.empty())
			{
				Part part{y, s.Shown == P_Struggle};
				y += pad;
				const std::string tonight = "tonight >";
				Txt(dl, ImVec2(x0, y), muted, res < 0 ? "WHY IT WAS LOST" : "WHY IT WAS WON", false, SmallSize());
				ImGui::SetCursorScreenPos(ImVec2(x0 + w - TextW(tonight, SmallSize()), y));
				if (ImGui::InvisibleButton("whytonight", ImVec2(TextW(tonight, SmallSize()), lh))) { Go(P_Struggle); s.StruggleNight = true; }
				Txt(dl, ImVec2(x0 + w - TextW(tonight, SmallSize()), y), ImGui::IsItemHovered() ? ink : kYou, tonight, false, SmallSize());
				y += lh;
				if (row("why", (lh + 2) * why.size(), &hov)) { Go(P_Struggle); s.StruggleNight = false; }
				if (hov) { TipWrapped(res < 0 ? "What cost the most players.\nClick: the whole view" : "Where this round beat a won fight its size.\nClick: the whole view"); }
				for (const StruggleWhy& k : why)
				{
					Mark(dl, ImVec2(x0 + lh * 0.35f, y + lh * 0.5f), lh * 0.28f, k.Verdict == 0 ? 1 : 2, k.Verdict == 0 ? kYou : k.Verdict == 1 ? kMid : kEnemy);
					StruggleIconAt(dl, ImVec2(x0 + lh * 0.9f, y), lh, k.Factor);
					Txt(dl, ImVec2(x0 + lh * 2.2f, y), ink, k.Label, false);
					Txt(dl, ImVec2(x0 + lh * 2.2f + TextW(k.Label, ImGui::GetFontSize()) + lh * 0.5f, y), muted, k.Value, false);
					y += lh + 2;
				}
				close(part);
			}
		}

		// ---- you: your down as steps, your rank on your spec ---------------------------------------------------------
		Part you{y, (s.Shown == P_Down && !x.DeathKey.empty() && s.DeathKey == x.DeathKey) || s.Shown == P_You};
		y += pad;
		// every way into You from here opens its overview for this round, not the measure or fix opened there last
		auto openYou = [&]() { Go(P_You); s.YouTonight = false; s.Measure = -1; s.Skill = kNoSkill; s.OpenFix = -1; };
		// the title opens You
		if (row("youtitle", lh, &hov)) { openYou(); }
		if (hov) { ImGui::SetTooltip("Click: you against your spec, this round"); }
		label("YOU", c.MeRaw ? c.MeRaw->Name + " \xc2\xb7 " + c.MeRaw->Spec + " \xc2\xb7 sg " + std::to_string(c.MeRaw->Subgroup) : std::string());
		if (row("youhead", lh, &hov))
		{
			if (x.WentDown) { Go(P_Down); s.DeathKey = x.DeathKey; s.DeathFilter = 1; s.DeathSpike = -1; }
			else { openYou(); }
		}
		if (hov && x.WentDown) { TipDown(x, f); }
		else if (hov && x.HasYou) { TipHits(x, f); }
		YouLine(L, ImVec2(x0, y), false);
		y += lh + 2;
		if (x.HasYou)
		{
			// downed, the steps: what took your stability, the last CC, the burst; not downed, what came at you: the CC,
			// the strips, the damage. Each with its hover.
			float sx = x0 + lh * 1.2f;
			bool first = true;
			const char* sep = x.WentDown ? ">" : "\xc2\xb7";
			// too long for the column (three steps, each with its time), the times go: they're in each step's hover
			bool when = true;
			{
				float need = sx - x0 + (x.WentDown ? TextW(">") + lh * 1.2f : 0);
				auto add = [&](const std::string& aText, const std::string& aWhen)
				{
					need += lh + 4 + TextW(aText) + (aWhen.empty() ? 0 : lh * 0.4f + TextW(aWhen, SmallSize())) + lh * 0.3f + TextW(sep) + lh * 0.3f;
				};
				if (x.WentDown)
				{
					if (x.StabStep) { add(x.StabHow, Rel(x.StabRel)); }
					if (x.CcStep) { add(std::string(Analysis::kCcVerbs[x.CcKind]) + ", no stab", Rel(x.CcRel)); }
					if (x.BurstDmg > 0) { add(Num(x.BurstDmg), "in " + Secs(x.BurstLen)); }
				}
				when = need <= w;
			}
			auto step = [&](const char* aId, const std::function<void(ImVec2)>& aIcon, const std::string& aText, const std::string& aWhenText, const std::function<void()>& aTip)
			{
				if (!first)
				{
					Txt(dl, ImVec2(sx, y), muted, sep, false);
					sx += TextW(sep) + lh * 0.3f;
				}
				first = false;
				const std::string aWhen = when ? aWhenText : std::string();
				float sw = lh + 4 + TextW(aText) + (aWhen.empty() ? 0 : lh * 0.4f + TextW(aWhen, SmallSize()));
				ImGui::SetCursorScreenPos(ImVec2(sx - 2, y));
				bool clicked = ImGui::InvisibleButton(aId, ImVec2(sw + 4, lh));
				if (ImGui::IsItemHovered())
				{
					dl->ChannelsSetCurrent(0);
					dl->AddRectFilled(ImVec2(sx - 2, y), ImVec2(sx + sw + 2, y + lh), kHoverBg);
					dl->ChannelsSetCurrent(1);
					aTip();
				}
				if (clicked && x.WentDown) { Go(P_Down); s.DeathKey = x.DeathKey; s.DeathFilter = 1; s.DeathSpike = -1; }
				else if (clicked) { openYou(); }
				aIcon(ImVec2(sx, y));
				Txt(dl, ImVec2(sx + lh + 4, y), ink, aText, false);
				if (!aWhen.empty()) { Txt(dl, ImVec2(sx + lh + 4 + TextW(aText) + lh * 0.4f, y + lh * 0.1f), muted, aWhen, false, SmallSize()); }
				sx += sw + lh * 0.3f;
			};
			if (x.WentDown)
			{
				if (x.StabStep)
				{
					step("stab", [&](ImVec2 p) { BoonIconAt(dl, p, lh, Analysis::kStability, true); }, x.StabHow == "never had any" ? "no stability" : x.StabHow,
						x.StabRel ? Rel(x.StabRel) : std::string(), [&]() { TipStab(x); });
				}
				if (x.CcStep)
				{
					Analysis::CcKind kind = x.CcKind;
					step("cc", [&](ImVec2 p) { CcIconAt(dl, p, lh, kind); }, std::string(Analysis::kCcVerbs[kind]) + (x.CcStabNote.rfind("no stability", 0) == 0 ? ", no stab" : ""),
						Rel(x.CcRel), [&]() { TipCc(x); });
				}
				if (x.BurstDmg > 0)
				{
					step("burst", [&](ImVec2 p) { IconAt(dl, p, lh, -303, "Damage burst", kEnemy); }, Num(x.BurstDmg), "in " + Secs(x.BurstLen), [&]() { TipHits(x, f); });
				}
				Txt(dl, ImVec2(sx, y), muted, ">", false);
				sx += TextW(">") + lh * 0.3f;
				if (x.Died) { Cross(dl, ImVec2(sx + lh * 0.4f, y + lh * 0.5f), lh * 0.24f, kEnemy); }
				else { Mark(dl, ImVec2(sx + lh * 0.4f, y + lh * 0.5f), lh * 0.28f, 1, kYou); }
			}
			else
			{
				if (x.CcTaken > 0)
				{
					Analysis::CcKind kind = x.CcMain;
					step("cc", [&](ImVec2 p) { CcIconAt(dl, p, lh, kind); }, "CC'd " + std::to_string(x.CcTaken) + (x.CcBare ? ", " + std::to_string(x.CcBare) + " no stab" : std::string()), "",
						[&]() { TipCounts(f, "CC on you this round: " + std::to_string(x.CcTaken) + ", " + std::to_string(x.CcBare) + " with no stability on", x.CcBy, "no stab"); });
				}
				if (x.StripsTaken > 0)
				{
					step("strips", [&](ImVec2 p) { BoonIconAt(dl, p, lh, Analysis::kStability, true); }, std::to_string(x.StripsTaken) + " stripped", "",
						[&]() { TipCounts(f, "Boons taken from you this round: " + std::to_string(x.StripsTaken) + ", " + std::to_string(x.StripsTaken - x.Corrupted) + " stripped and " +
							std::to_string(x.Corrupted) + " corrupted (made a condition); " + std::to_string(x.StabStripped) + " of them stability", x.StripBy, "stability"); });
				}
				step("taken", [&](ImVec2 p) { IconAt(dl, p, lh, -303, "Damage taken", kEnemy); }, Num(double(x.DamageTaken)) + " taken", "", [&]() { TipHits(x, f); });
			}
			y += lh + 2;
		}
		if (x.HasYou)
		{
			// the line opens You (why), its "compare >" opens Compare: you against the best on your spec, anyone else
			// picked there
			bool clicked = row("rank", lh, &hov);
			const float mx = ImGui::GetIO().MousePos.x, cmpX = CompareLinkX(x0 + w), whyX = WhyLinkX(x0 + w);
			const bool onCompare = hov && mx >= cmpX - 2 && mx < whyX - lh * 0.3f;
			if (clicked && onCompare) { Go(P_Compare); s.CompareLeft.clear(); s.CompareRight.clear(); s.Metric = -1; s.CompareTonight = false; }
			else if (clicked) { openYou(); }
			if (onCompare) { TipWrapped("Compare: you against the best on your spec, skill by skill on your main measure; pick anyone else and any measure there"); }
			else if (hov) { TipRank(x, c); }
			RankLine(L, ImVec2(x0, y), true, onCompare ? 1 : hov && mx >= whyX - 2 ? 2 : 0);
			y += lh + 2;
		}
		close(you);

		// ---- the calls: every one, a click opens who and when --------------------------------------------------------
		const std::vector<CallCard>& calls = RoundCalls(f);
		Part callsPart{y, s.Shown == P_Calls};
		y += pad;
		{
			int n[3] = {0, 0, 0};
			for (const CallCard& k : calls) { n[std::clamp(k.Kind, 0, 2)]++; }
			std::string right = std::to_string(n[0]) + " on \xc2\xb7 " + std::to_string(n[1]) + " partly \xc2\xb7 " + std::to_string(n[2]) + " off";
			if (row("callslabel", lh, &hov)) { Go(P_Calls); }
			if (hov) { ImGui::SetTooltip("Each key skill your squad brought, judged by its own rule.\nClick: every call, who and when"); }
			label("CALLS", calls.empty() ? std::string() : right);
		}
		if (calls.empty())
		{
			Txt(dl, ImVec2(x0, y), muted, "No key skills (wells, bursts, Tale, stability...) this round", false);
			y += lh;
		}
		else
		{
			// the chips, off first: a mark, the name, the verdict when it isn't on, the count
			std::vector<const CallCard*> order;
			for (const CallCard& k : calls) { order.push_back(&k); }
			std::stable_sort(order.begin(), order.end(), [](const CallCard* a, const CallCard* b) { return a->Kind > b->Kind; });
			auto shortName = [](const std::string& aName)
			{
				if (aName == "Tale of the August Queen") { return std::string("Tale"); }
				if (aName == "Winds of Disenchantment") { return std::string("Winds"); }
				if (aName == "Chronomancer burst") { return std::string("Chrono burst"); }
				if (aName == "Necromancer burst") { return std::string("Necro burst"); }
				if (aName == "Elementalist burst") { return std::string("Ele burst"); }
				if (aName == "Continuum Split") { return std::string("Continuum"); }
				return aName;
			};
			float cx = x0;
			const float ch = lh + 4;
			for (const CallCard* k : order)
			{
				std::string name = shortName(k->Name), verdict = k->Kind ? k->Verdict : std::string(), count = std::to_string(k->Num) + "/" + std::to_string(k->Den);
				float cw = lh * 0.9f + TextW(name) + (verdict.empty() ? 0 : lh * 0.35f + TextW(verdict, SmallSize())) + lh * 0.35f + TextW(count, SmallSize()) + lh * 0.6f;
				if (cx + cw > x0 + w && cx > x0) { cx = x0; y += ch + 3; }
				ImGui::SetCursorScreenPos(ImVec2(cx, y));
				ImGui::PushID(k->Key.c_str());
				bool clicked = ImGui::InvisibleButton("##call", ImVec2(cw, ch));
				bool h2 = ImGui::IsItemHovered();
				ImGui::PopID();
				dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + cw, y + ch), h2 ? kHoverBg : IM_COL32(0x18, 0x1b, 0x20, 255));
				dl->AddRect(ImVec2(cx, y), ImVec2(cx + cw, y + ch), s.Shown == P_Calls && s.CallOpen == k->Key ? kYou : IM_COL32(0x2c, 0x31, 0x3a, 255));
				float tx = cx + lh * 0.3f;
				Verdict(dl, ImVec2(tx + lh * 0.25f, y + ch * 0.5f), lh * 0.24f, std::clamp(k->Kind, 0, 2));
				tx += lh * 0.6f;
				Txt(dl, ImVec2(tx, y + 2), ink, name, false);
				tx += TextW(name) + lh * 0.35f;
				if (!verdict.empty()) { Txt(dl, ImVec2(tx, y + 2 + lh * 0.08f), muted, verdict, false, SmallSize()); tx += TextW(verdict, SmallSize()) + lh * 0.35f; }
				Txt(dl, ImVec2(tx, y + 2 + lh * 0.08f), ImGui::GetColorU32(ImGuiCol_TextDisabled), count, false, SmallSize());
				if (h2) { TipCall(*k); }
				if (clicked) { Go(P_Calls); s.CallOpen = k->Key; }
				cx += cw + 3;
			}
			y += ch;
		}
		close(callsPart);

		// ---- downs: the squad as the game's squad window, a row per subgroup -----------------------------------------
		Part downs{y, s.Shown == P_Downs || (s.Shown == P_Down && s.DeathKey != x.DeathKey) || s.Shown == P_Revives};
		y += pad;
		{
			std::string head = "DOWNS " + std::to_string(x.SquadDowns) + " \xc2\xb7 DIED " + std::to_string(x.SquadDeaths);
			Txt(dl, ImVec2(x0, y), muted, head, false, SmallSize());
			std::string all = "all " + std::to_string(x.SquadDowns) + " downs >";
			ImGui::SetCursorScreenPos(ImVec2(x0 + w - TextW(all, SmallSize()), y));
			if (ImGui::InvisibleButton("alldowns", ImVec2(TextW(all, SmallSize()), lh))) { Go(P_Downs); s.DeathFilter = 0; s.DeathSpike = -1; }
			Txt(dl, ImVec2(x0 + w - TextW(all, SmallSize()), y), ImGui::IsItemHovered() ? ink : kYou, all, false, SmallSize());
			y += lh;
		}
		{
			const float cell = std::floor(lh * 0.95f), cgap = std::floor(lh * 0.3f);
			const float labelW = lh * 1.7f;
			const float gy0 = y;
			for (const auto& r : x.Grid)
			{
				std::string sg = std::to_string(r.Sg);
				Txt(dl, ImVec2(x0, y + (cell - SmallSize()) * 0.5f), muted, sg, false, SmallSize());
				if (r.Cmd) { Mark(dl, ImVec2(x0 + TextW(sg, SmallSize()) + lh * 0.35f, y + cell * 0.5f), lh * 0.22f, 2, kMid); }
				float cx = x0 + labelW;
				for (const Player* p : r.Members)
				{
					auto it = x.DownsOf.find(p);
					int nDowns = it == x.DownsOf.end() ? 0 : static_cast<int>(it->second.size());
					bool died = false;
					if (it != x.DownsOf.end()) { for (const DownInfo& d : it->second) { died |= d.Died; } }
					ImGui::SetCursorScreenPos(ImVec2(cx, y));
					ImGui::PushID(p);
					bool clicked = ImGui::InvisibleButton("##p", ImVec2(cell, cell));
					bool h2 = ImGui::IsItemHovered();
					ImGui::PopID();
					if (nDowns == 0) { dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + cell, y + cell), IM_COL32(0x16, 0x18, 0x1c, 255)); }
					SpecIconAt(dl, ImVec2(cx, y), cell, *p);
					if (nDowns == 0) { dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + cell, y + cell), IM_COL32(0x0f, 0x10, 0x13, 150)); } // faded: not downed
					if (p->Pov) { dl->AddRect(ImVec2(cx - 1, y - 1), ImVec2(cx + cell + 1, y + cell + 1), ink); }
					if (died) { Cross(dl, ImVec2(cx + cell - 2, y + cell - 2), lh * 0.18f, kEnemy); }
					else if (nDowns > 0)
					{
						Mark(dl, ImVec2(cx + cell - 2, y + cell - 2), lh * 0.2f, 4, IM_COL32(0xf3, 0xa0, 0x7c, 255));
						if (nDowns > 1) { Txt(dl, ImVec2(cx + cell + 1, y + cell * 0.25f), IM_COL32(0xf3, 0xa0, 0x7c, 255), std::to_string(nDowns), true, SmallSize() * 0.85f); }
					}
					if (h2) { dl->AddRect(ImVec2(cx - 2, y - 2), ImVec2(cx + cell + 2, y + cell + 2), kYou); TipPlayer(x, *p); }
					if (clicked)
					{
						if (it != x.DownsOf.end())
						{
							const DownInfo* d = &it->second.back();
							for (const DownInfo& e : it->second) { if (e.Died) { d = &e; } }
							Go(P_Down);
							s.DeathKey = d->Key;
							// Earlier and Later step through this player's downs
							s.DeathFilter = 5;
							s.DeathPlayer = p->Account;
							s.DeathSpike = -1;
						}
						else { Go(P_Compare); s.CompareLeft = p->Account; s.CompareRight.clear(); s.Metric = -1; }
					}
					cx += cell + cgap;
				}
				y += cell + 2;
			}
			// beside the grid: the two subgroups hit hardest, the weakest cover, how many fell in their spikes
			float fx = x0 + labelW + 5 * (cell + cgap) + lh * 0.8f, fw = std::max(lh, x0 + w - fx);
			float fy = gy0;
			auto fact = [&](const char* aId, const std::string& aText, ImU32 aCol, const std::function<void()>& aTip, const std::function<void()>& aClick)
			{
				ImGui::SetCursorScreenPos(ImVec2(fx, fy));
				bool clicked = ImGui::InvisibleButton(aId, ImVec2(fw, lh));
				if (ImGui::IsItemHovered() && aTip) { aTip(); }
				if (clicked && aClick) { aClick(); }
				dl->PushClipRect(ImVec2(fx, fy), ImVec2(fx + fw, fy + lh), true);
				Txt(dl, ImVec2(fx, fy), aCol, aText, false);
				dl->PopClipRect();
				fy += lh + 1;
			};
			for (const auto& sg : x.Worst)
			{
				int g = sg.Sg;
				fact(("sg" + std::to_string(g)).c_str(), "sg " + std::to_string(g) + (sg.Cmd ? " (cmd)" : "") + ": " + std::to_string(sg.Downs) + Plural(" down", sg.Downs) + ", " + std::to_string(sg.Died) + " died",
					ink, [&x, g]() { TipSg(x, g); }, [&s]() { Go(P_Downs); s.DeathFilter = 0; s.DeathSpike = -1; });
			}
			if (x.CoverSg >= 0)
			{
				std::string t2 = "sg " + std::to_string(x.CoverSg) + " CC covered " + std::to_string(x.CoverHit) + " of " + std::to_string(x.CoverWin);
				fact("cover", t2, ink, [&x]()
				{
					ImGui::SetTooltip("Subgroup %d: %d of %d CC had someone's stability on them, the weakest cover this round.\nThe squad: %d of %d.", x.CoverSg, x.CoverHit, x.CoverWin, x.CcHit, x.CcWin);
				}, [&s]() { Go(P_Round); s.SpikeOpen = -1; s.RoundView = 1; });
			}
			fact("inspikes", std::to_string(x.InTheirSpikes) + " in enemy spikes \xc2\xb7 " + std::to_string(x.StrippedFirst) + " stripped first", muted, [&x]()
			{
				ImGui::SetTooltip("%d of %d downs came in enemy spikes (3 s before a peak to 4 s after).\n%d of %d had their stability stripped or corrupted in the 6 s before going down.", x.InTheirSpikes, x.SquadDowns, x.StrippedFirst, x.SquadDowns);
			}, nullptr);
			y = std::max(y, fy) + 2;
		}
		close(downs);

		// ---- the enemy: who, their worst spike, the class behind their spike damage ----------------------------------
		Part enemy{y, s.Shown == P_Round && s.SpikeOpen >= 0 && s.SpikeEnemy};
		y += pad;
		{
			std::string head = "ENEMY " + std::to_string(x.EnemyTotal);
			Txt(dl, ImVec2(x0, y), muted, head, false, SmallSize());
			const bool split = x.Teams.size() >= 2;
			if (!x.Teams.empty() && x.Teams[0].Team >= 0)
			{
				// which team: one, or how many when two or more fought us
				std::string which = split ? std::to_string(x.Teams.size()) + " TEAMS" : std::string(TeamName(x.Teams[0].Team));
				for (char& ch : which) { ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); }
				Txt(dl, ImVec2(x0 + TextW(head + " ", SmallSize()), y), muted, "\xc2\xb7 " + which, false, SmallSize());
			}
			// the label copies the enemy squad as one line, for chat (the Summary's copy)
			static double copiedAt = -10;
			ImGui::SetCursorScreenPos(ImVec2(x0, y));
			if (ImGui::InvisibleButton("copy", ImVec2(TextW(head, SmallSize()), lh))) { ImGui::SetClipboardText(x.CopyLine.c_str()); copiedAt = ImGui::GetTime(); }
			if (ImGui::GetTime() - copiedAt < 2.5) { ImGui::SetTooltip("Copied: %s", x.CopyLine.c_str()); }
			else if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Click: copy the enemy squad as one line\n%s", x.CopyLine.c_str()); }
			// the classes as icons with their count; split by team, a line per team ("Green 53", then its classes)
			auto icons = [&](const std::vector<Facts::Spec>& aSpecs, float aX, const char* aTeam)
			{
				float cx = aX;
				for (const auto& e : aSpecs)
				{
					std::string n = std::to_string(e.Count);
					float iw = lh * 0.9f + 2 + TextW(n, SmallSize()) + lh * 0.45f;
					if (cx + iw > x0 + w) { break; }
					SpecIconAt(dl, ImVec2(cx, y), lh * 0.9f, e.Icon);
					Txt(dl, ImVec2(cx + lh * 0.9f + 2, y), ink, n, false, SmallSize());
					if (ImGui::IsMouseHoveringRect(ImVec2(cx, y), ImVec2(cx + iw, y + lh)))
					{
						ImGui::SetTooltip(aTeam ? "%d %s (%s)" : "%d %s", e.Count, Plural(e.Name, e.Count).c_str(), aTeam ? aTeam : "");
					}
					cx += iw;
				}
			};
			if (!split)
			{
				float hx = x0 + TextW(head, SmallSize()) + lh * 0.6f;
				if (!x.Teams.empty() && x.Teams[0].Team >= 0) { hx += TextW(std::string("\xc2\xb7 ") + TeamName(x.Teams[0].Team) + " ", SmallSize()); }
				icons(x.Enemy, hx, nullptr);
				y += lh + 1;
			}
			else
			{
				y += lh;
				float labelW = 0;
				for (const auto& t : x.Teams) { labelW = std::max(labelW, TextW(std::string(TeamName(t.Team)) + " " + std::to_string(t.Count))); }
				for (const auto& t : x.Teams)
				{
					std::string teamLabel = std::string(TeamName(t.Team)) + " " + std::to_string(t.Count);
					Txt(dl, ImVec2(x0, y), ink, teamLabel, false);
					icons(t.Specs, x0 + labelW + lh * 0.6f, TeamName(t.Team));
					y += lh + 1;
				}
			}
		}
		if (x.WorstZone >= 0)
		{
			const Zone& z = x.Zones[x.WorstZone];
			if (row("worst", lh, &hov)) { OpenSpike(f, z, false); }
			bool rowHov = hov;
			std::string t1 = "Worst spike " + Duration(z.TheirMs);
			Txt(dl, ImVec2(x0, y), ink, t1, false);
			float cx = x0 + TextW(t1) + lh * 0.5f;
			std::string dn = "-" + std::to_string(z.Downs.size()) + (z.Died ? "  x" + std::to_string(z.Died) : "");
			Mark(dl, ImVec2(cx + lh * 0.3f, y + lh * 0.5f), lh * 0.28f, 4, kEnemy);
			cx += lh * 0.7f;
			std::string dn2 = std::to_string(z.Downs.size());
			Txt(dl, ImVec2(cx, y), kEnemy, dn2, false);
			cx += TextW(dn2) + lh * 0.3f;
			if (z.Died)
			{
				Cross(dl, ImVec2(cx + lh * 0.3f, y + lh * 0.5f), lh * 0.22f, kEnemy);
				cx += lh * 0.7f;
				Txt(dl, ImVec2(cx, y), kEnemy, std::to_string(z.Died), false);
				cx += TextW(std::to_string(z.Died)) + lh * 0.3f;
			}
			cx += lh * 0.4f;
			bool skillHov = false;
			for (const SkillSum& sk : z.TheirTop)
			{
				std::string v = Num(double(sk.Damage));
				float iw = lh + 3 + TextW(v, SmallSize()) + lh * 0.5f;
				if (cx + iw > x0 + w) { break; }
				IconAt(dl, ImVec2(cx, y), lh, sk.Skill, sk.Name);
				Txt(dl, ImVec2(cx + lh + 3, y + lh * 0.08f), muted, v, false, SmallSize());
				if (ImGui::IsMouseHoveringRect(ImVec2(cx, y), ImVec2(cx + iw, y + lh)))
				{
					skillHov = true;
					ImGui::BeginTooltip();
					ImGui::Text("%s", sk.Name.c_str());
					ImGui::TextColored(kMuted, "%s \xc2\xb7 %d hits \xc2\xb7 %s in the 6 s around %s", sk.Who.c_str(), sk.Hits, v.c_str(), Duration(z.TheirMs).c_str());
					ImGui::EndTooltip();
				}
				cx += iw;
			}
			if (rowHov && !skillHov) { TipZone(f, z, false); }
			y += lh + 2;
		}
		if (!x.TopSpec.empty())
		{
			if (row("topspec", lh, &hov)) { if (x.WorstZone >= 0) { OpenSpike(f, x.Zones[x.WorstZone], false); } }
			if (hov)
			{
				ImGui::BeginTooltip();
				ImGui::Text("%d %s of %d enemies dealt %s of the %s the %d enemy spikes did to allies", x.TopSpecCount, Plural(x.TopSpec, x.TopSpecCount).c_str(), x.EnemyTotal,
					Num(double(x.TopSpecDamage)).c_str(), Num(double(x.SpikeDamage)).c_str(), static_cast<int>(f.TheirSpikesMs.size()));
				SkillRows(f, x.TopSpecSkills, false);
				ImGui::TextColored(kMuted, "Each spike: enemy damage to allies in the 6 s around its peak");
				ImGui::EndTooltip();
			}
			std::string t1 = std::to_string(x.TopSpecCount) + " " + Plural(x.TopSpec, x.TopSpecCount) + " dealt " + Pct(100 * x.TopSpecShare) + " of enemy spike damage";
			Txt(dl, ImVec2(x0, y), ink, t1, false);
			y += lh + 2;
		}
		close(enemy);

		// ---- the best this round: two columns, what got them there on hover -----------------------------------------
		Part best{y, s.Shown == P_Compare || s.Shown == P_Squad};
		y += pad;
		{
			Txt(dl, ImVec2(x0, y), muted, "BEST THIS ROUND", false, SmallSize());
			std::string all = "all " + std::to_string(f.SquadCount) + " players >";
			ImGui::SetCursorScreenPos(ImVec2(x0 + w - TextW(all, SmallSize()), y));
			if (ImGui::InvisibleButton("allplayers", ImVec2(TextW(all, SmallSize()), lh))) { Go(P_Squad); }
			Txt(dl, ImVec2(x0 + w - TextW(all, SmallSize()), y), ImGui::IsItemHovered() ? ink : kYou, all, false, SmallSize());
			y += lh;
		}
		{
			const float colW = (w - lh * 0.8f) * 0.5f;
			float labelW = 0; // the widest job name, so every name starts in line
			for (const Best& b : x.Bests) { labelW = std::max(labelW, TextW(b.Label, SmallSize())); }
			for (size_t i = 0; i < x.Bests.size(); i++)
			{
				const Best& b = x.Bests[i];
				float bx = x0 + (i % 2) * (colW + lh * 0.8f), by = y + (i / 2) * (lh + 2);
				ImGui::SetCursorScreenPos(ImVec2(bx - 2, by));
				ImGui::PushID(static_cast<int>(i));
				bool clicked = ImGui::InvisibleButton("##best", ImVec2(colW + 4, lh));
				bool h2 = ImGui::IsItemHovered();
				ImGui::PopID();
				if (h2)
				{
					dl->ChannelsSetCurrent(0);
					dl->AddRectFilled(ImVec2(bx - 2, by), ImVec2(bx + colW + 2, by + lh), kHoverBg);
					dl->ChannelsSetCurrent(1);
					TipBest(f, b);
				}
				if (clicked) { Go(P_Compare); s.CompareLeft = b.P->Account; s.CompareRight.clear(); s.Metric = b.Metric; s.CompareTonight = false; }
				Txt(dl, ImVec2(bx, by + (lh - SmallSize()) * 0.5f), muted, b.Label, false, SmallSize());
				float nx = bx + labelW + lh * 0.4f;
				SpecIconAt(dl, ImVec2(nx, by), lh * 0.9f, *b.P);
				float vx = bx + colW - TextW(b.Value);
				dl->PushClipRect(ImVec2(nx + lh, by), ImVec2(vx - lh * 0.5f, by + lh), true);
				Txt(dl, ImVec2(nx + lh, by), ink, b.P->Name, false);
				dl->PopClipRect();
				Txt(dl, ImVec2(vx, by), ink, b.Value, false);
			}
			y += ((x.Bests.size() + 1) / 2) * (lh + 2);
		}
		close(best);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(o);
		ImGui::Dummy(ImVec2(W, y - o.y));
	}

	float SmallWindow(const Ctx& c, float aWidth, unsigned aLines, bool aShadow, bool aStrip)
	{
		const Fight& f = *c.F;
		const Facts& x = FactsOf(c);
		State& s = S();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float lh = ImGui::GetTextLineHeight();
		const ImVec2 o = ImGui::GetCursorScreenPos();
		const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
		float y = o.y;
		LineCtx L{&x, &c, dl, aWidth, aShadow};
		// a click opens the window on the latest round at that pane
		auto open = [&](int aPane)
		{
			ShowWindow = true;
			s.Selected = -1;
			s.KeepPane = true;
			Go(aPane);
		};
		bool hov = false;
		auto row = [&](const char* aId) -> bool
		{
			ImGui::SetCursorScreenPos(ImVec2(o.x, y));
			bool clicked = ImGui::InvisibleButton(aId, ImVec2(aWidth, lh));
			hov = ImGui::IsItemHovered();
			if (hov) { dl->AddRectFilled(ImVec2(o.x - 2, y), ImVec2(o.x + aWidth + 2, y + lh), IM_COL32(0x14, 0x18, 0x1e, 160)); }
			return clicked;
		};
		// the result: LOST 8 : 30, the length, when it started
		if (row("score")) { open(P_Round); s.SpikeOpen = -1; s.RoundView = 0; }
		if (hov) { TipScore(x, f); }
		ScoreLine(x, ImVec2(o.x, y), aShadow, 1.0f);
		{
			std::string when = x.Length;
			Txt(dl, ImVec2(o.x + aWidth - TextW(when), y), muted, when, aShadow);
		}
		y += lh + 2;
		if (aStrip)
		{
			const float sh = lh * 1.3f + 2 * (SmallSize() + 2);
			Strip(x, f, c.MeRaw, ImVec2(o.x, y), aWidth, sh, aShadow, true);
			y += sh + 3;
		}
		if ((aLines & (1u << ML_You)) && x.HasYou)
		{
			if (row("you")) { if (x.WentDown) { open(P_Down); s.DeathKey = x.DeathKey; s.DeathFilter = 1; s.DeathSpike = -1; } else { open(P_You); s.YouTonight = false; } }
			if (x.WentDown)
			{
				// you as steps in one line: "x You 0:13", then the steps as icons"
				float cx = o.x;
				if (x.Died) { Cross(dl, ImVec2(cx + lh * 0.4f, y + lh * 0.5f), lh * 0.22f, kEnemy); } else { Mark(dl, ImVec2(cx + lh * 0.4f, y + lh * 0.5f), lh * 0.28f, 4, kEnemy); }
				cx += lh;
				std::string head = "You " + Duration(x.DownMs);
				Txt(dl, ImVec2(cx, y), ink, head, aShadow);
				cx += TextW(head) + lh * 0.5f;
				// the steps as icons with arrows, their words on hover
				std::vector<ChainStep> steps;
				if (x.StabStep)
				{
					ChainStep st;
					st.Boon = Analysis::kStability;
					st.Name = x.StabHow == "never had any" ? "No stability" : "Stability " + x.StabHow;
					if (x.StabRel) { st.Tip = Rel(x.StabRel) + " before the down"; }
					steps.push_back(st);
				}
				if (x.CcStep)
				{
					ChainStep st;
					st.Cc = x.CcKind;
					st.Name = std::string(Analysis::kCcVerbs[x.CcKind]) + " by " + x.CcSkillName;
					st.Tip = Rel(x.CcRel) + " before the down; " + x.CcStabNote;
					steps.push_back(st);
				}
				if (x.BurstDmg > 0)
				{
					ChainStep st;
					st.Skill = -303;
					st.Name = "Damage burst";
					st.Value = Num(x.BurstDmg);
					st.Tip = "to health in " + Secs(x.BurstLen);
					steps.push_back(st);
				}
				dl->PushClipRect(ImVec2(cx, y), ImVec2(o.x + aWidth, y + lh), true);
				float sw = ChainAt(dl, ImVec2(cx, y), lh, steps);
				dl->PopClipRect();
				if (ImGui::IsMouseHoveringRect(ImVec2(cx, y), ImVec2(cx + sw, y + lh))) { hov = false; } // the icon's own words, not the line's
				cx += sw + lh * 0.4f;
				if (!x.Died) { Mark(dl, ImVec2(cx + lh * 0.3f, y + lh * 0.5f), lh * 0.26f, 1, kYou); }
			}
			else { YouLine(L, ImVec2(o.x, y)); }
			if (hov) { if (x.WentDown) { TipDown(x, *c.F); } else { TipHits(x, f); } }
			y += lh + 1;
		}
		if ((aLines & (1u << ML_Rank)) && x.HasYou)
		{
			if (row("rank")) { open(P_You); s.YouTonight = false; s.Measure = -1; s.Skill = kNoSkill; s.OpenFix = -1; } // You's overview, as the big window
			if (hov) { TipRank(x, c); }
			RankLine(L, ImVec2(o.x, y), false);
			y += lh + 1;
		}
		if (aLines & (1u << ML_Why))
		{
			// the round's top cause, as the big window's "why it was lost / won"
			std::vector<StruggleWhy> why;
			const int res = StruggleTop(c, why);
			if (!why.empty())
			{
				if (row("why")) { open(P_Struggle); s.StruggleNight = false; }
				if (hov) { TipWrapped(res < 0 ? "What cost the most players.\nClick: why this round was lost" : "Where this round beat a won fight its size.\nClick: why this round was won"); }
				Reason r;
				r.Kind = res < 0 ? 2 : 0;
				r.Short = why[0].Label + ": " + why[0].Value;
				ReasonLine(L, ImVec2(o.x, y), r, true);
				y += lh + 1;
			}
		}
		if (aLines & (1u << ML_Downs))
		{
			if (row("downs")) { open(P_Downs); s.DeathFilter = 0; s.DeathSpike = -1; }
			if (hov && !x.Worst.empty()) { TipSg(x, x.Worst[0].Sg); }
			Mark(dl, ImVec2(o.x + lh * 0.4f, y + lh * 0.5f), lh * 0.28f, 4, kEnemy);
			std::string t1 = std::to_string(x.SquadDowns) + " down \xc2\xb7 " + std::to_string(x.SquadDeaths) + " died";
			Txt(dl, ImVec2(o.x + lh, y), ink, t1, aShadow);
			if (!x.Worst.empty())
			{
				std::string t2 = "most sg " + std::to_string(x.Worst[0].Sg) + (x.Worst[0].Cmd ? " (cmd)" : "") + " (" + std::to_string(x.Worst[0].Downs) + ")";
				Txt(dl, ImVec2(o.x + lh + TextW(t1) + lh * 0.5f, y), muted, t2, aShadow);
			}
			y += lh + 1;
		}
		if ((aLines & (1u << ML_Enemy)) && x.WorstZone >= 0)
		{
			const Zone& z = x.Zones[x.WorstZone];
			if (row("enemy")) { ShowWindow = true; s.Selected = -1; s.KeepPane = true; OpenSpike(f, z, false); }
			if (hov) { TipZone(f, z, false); }
			float cx = o.x;
			if (!x.TopSpec.empty()) { SpecIconAt(dl, ImVec2(cx, y), lh * 0.9f, SpecPlayer(x.TopSpec)); cx += lh + 2; }
			std::string t1 = Duration(z.TheirMs) + " -" + std::to_string(z.Downs.size());
			Txt(dl, ImVec2(cx, y), ink, t1, aShadow);
			cx += TextW(t1) + lh * 0.5f;
			if (!x.TopSpec.empty())
			{
				std::string t2 = std::to_string(x.TopSpecCount) + " " + Plural(x.TopSpec, x.TopSpecCount) + ": " + Pct(100 * x.TopSpecShare) + " of enemy spike damage";
				dl->PushClipRect(ImVec2(cx, y), ImVec2(o.x + aWidth, y + lh), true);
				Txt(dl, ImVec2(cx, y), muted, t2, aShadow);
				dl->PopClipRect();
			}
			y += lh + 1;
		}
		if ((aLines & (1u << ML_Best)) && !x.Bests.empty())
		{
			const Best& b = x.Bests[0];
			if (row("best")) { open(P_Compare); s.CompareLeft = b.P->Account; s.CompareRight.clear(); s.Metric = b.Metric; }
			if (hov) { TipBest(f, b); }
			Mark(dl, ImVec2(o.x + lh * 0.4f, y + lh * 0.5f), lh * 0.26f, 1, kYou);
			SpecIconAt(dl, ImVec2(o.x + lh, y), lh * 0.9f, *b.P);
			std::string t1 = b.P->Name;
			Txt(dl, ImVec2(o.x + lh * 2 + 2, y), ink, t1, aShadow);
			std::string t2 = b.Value + " " + (b.Label == "Damage" ? "damage" : b.Label);
			Txt(dl, ImVec2(o.x + lh * 2 + 2 + TextW(t1) + lh * 0.5f, y), muted, t2, aShadow);
			y += lh + 1;
		}
		if (aLines & (1u << ML_Calls))
		{
			const std::vector<CallCard>& calls = RoundCalls(f);
			if (!calls.empty())
			{
				int n[3] = {0, 0, 0};
				std::string off;
				for (const CallCard& k : calls)
				{
					n[std::clamp(k.Kind, 0, 2)]++;
					if (k.Kind == 2) { off += (off.empty() ? "" : ", ") + k.Name; }
				}
				if (row("calls")) { open(P_Calls); }
				if (hov) { ImGui::SetTooltip("Calls: %d on, %d partly, %d off%s\nClick: who and when", n[0], n[1], n[2], off.empty() ? "" : ("\noff: " + off).c_str()); }
				Mark(dl, ImVec2(o.x + lh * 0.4f, y + lh * 0.5f), lh * 0.26f, n[2] ? 4 : n[1] ? 2 : 1, n[2] ? kEnemy : n[1] ? kMid : kYou);
				std::string t1 = "Calls " + std::to_string(n[2]) + " off";
				Txt(dl, ImVec2(o.x + lh, y), ink, t1, aShadow);
				dl->PushClipRect(ImVec2(o.x + lh + TextW(t1) + lh * 0.5f, y), ImVec2(o.x + aWidth, y + lh), true);
				Txt(dl, ImVec2(o.x + lh + TextW(t1) + lh * 0.5f, y), muted, off.empty() ? std::to_string(n[1]) + " partly, " + std::to_string(n[0]) + " on" : off, aShadow);
				dl->PopClipRect();
				y += lh + 1;
			}
		}
		ImGui::SetCursorScreenPos(o);
		ImGui::Dummy(ImVec2(aWidth, y - o.y));
		return y - o.y;
	}

	void HelpView()
	{
		struct Row { const char* Part; const char* Text; };
		static const Row kRows[] = {
			{"Round", "The result (enemies downed : allies downed), the strip and CC both ways. The strip: ally damage to enemy players per "
				"second above the line, enemy damage below; shaded: spikes; a triangle up and a number above: enemies downed in that ally "
				"spike, a triangle down below: allies downed in that enemy spike; x you died, a white triangle: you were downed. Hover a "
				"spike for who went down and both sides' top three skills, click it for the spike broken down: the upper half opens the ally "
				"spike, the lower half the enemy one. Below it: how much enemy CC stability blocked on allies, how much ally CC landed, "
				"against the middle of tonight's rounds, until allies stopped fighting (lost: the worse first; won: the better)."},
			{"Why it was lost / won", "Lost: the two causes that cost the most players against won fights of this size (15v15 to "
				"40v40+), with the count. Won: how many downed allies and enemies died. Click for the whole why view: every cause, this "
				"round against won fights, who."},
			{"You", "Your down as steps: hover each for what took your stability, what CC'd you and what hit you; click for the whole of "
				"it. Your rank: your spec's main measure against the others on your spec. why >: your measures and what to fix first; "
				"compare >: you against the best on your spec, skill by skill (pick anyone else there)."},
			{"Calls", "Each key skill your squad brought, judged by its own rule (wells together, bursts with the wells, Winds one or two "
				"per spike...). Hover for the count and who was off, click for who and when. Skills as icons: struck through in red, left "
				"out; in grey, down or on cooldown; a gold frame, off time; hover an icon for its name and when."},
			{"Downs", "The squad as the game's squad window: a row per subgroup (the gold diamond: the commander's). x died, a triangle: "
				"downed (and how often); faded: not downed. Hover a player for what happened; click for their down (Earlier and Later "
				"step through their downs), or to compare them when they weren't downed. Every player: all players >, a name opens Compare."},
			{"Enemy", "The enemy classes (click the label to copy them as one line), the enemy spike that downed the most allies with its top "
				"skills, and the class that did most of the enemy spike damage."},
			{"Best this round", "The best of the squad in each job; hover for how they got there, click to compare them with the next best "
				"on their spec."},
			{"The right side", "Whatever you click opens here; Back returns to what was open before. Night: your night, the fixes that "
				"keep coming back."}};
		if (ImGui::BeginTable("help", 2, ImGuiTableFlags_SizingFixedFit))
		{
			float first = 0;
			for (const Row& r : kRows) { first = std::max(first, ImGui::CalcTextSize(r.Part).x); }
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, first + ImGui::GetFontSize());
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
			for (const Row& r : kRows)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0); ImGui::TextColored(kMuted, "%s", r.Part);
				ImGui::TableSetColumnIndex(1);
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(r.Text);
				ImGui::PopTextWrapPos();
				ImGui::Spacing();
			}
			ImGui::EndTable();
		}
	}

	// The calls (the Summary's cards, picked by hand A3 with A2's detail in place): a card per key skill this squad
	// brought, judged by its own rule; a card opens who and when right under its row of cards
	void CallsView(const Ctx& c)
	{
		State& s = S();
		const float lh = ImGui::GetTextLineHeight();
		const std::vector<CallCard>& calls = RoundCalls(*c.F);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
		auto colOf = [&](int aKind) { return aKind == 0 ? kYou : aKind == 1 ? kMid : kEnemy; };
		auto shapeOf = [](int aKind) { return aKind == 0 ? 1 : aKind == 1 ? 2 : 4; }; // circle, diamond, down: never colour alone
		if (calls.empty())
		{
			ImGui::TextColored(kMuted, "None of the key skills (wells, bursts, Tale, stability, revives...) came up this round.");
			return;
		}
		int n[3] = {0, 0, 0};
		for (const CallCard& k : calls) { n[std::clamp(k.Kind, 0, 2)]++; }
		Answer("Calls: " + std::to_string(n[0]) + " on, " + std::to_string(n[1]) + " partly, " + std::to_string(n[2]) + " off");
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x;
		float y = at.y;
		const char* groups[] = {"IN ALLY SPIKES", "BEFORE ENEMY SPIKES", "AFTER DOWNS, AND BOTH WAYS"};

		// who and when for one card: its whole line, then its rows, under a heading per part (a Warrior's opener and
		// melee burst) and a labelled divider per spike or call, so rows of different spikes don't run together. How the
		// rows are judged is the head's hover, a part's its heading's.
		auto detail = [&](const CallCard& k)
		{
			const float aX = at.x, aW = width;
			float top = y;
			y += 4;
			std::string head = k.Name + ": " + k.Line;
			dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(aX + 6, y), ink, head.c_str(), nullptr, aW - 12);
			float headH = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, aW - 12, head.c_str()).y;
			if (!k.Detail.empty() && ImGui::IsMouseHoveringRect(ImVec2(aX + 6, y), ImVec2(aX + aW - 6, y + headH))) { TipWrapped(k.Detail); }
			y += headH + 6;
			// a table card (Tale, stability): a heading per column with its rule on hover, numbers right-aligned, each cell's
			// story on its hover
			if (!k.Head.empty())
			{
				const size_t nc = k.Head.size();
				const float small = SmallSize();
				auto headW = [&](size_t i) { return ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, k.Head[i].c_str()).x; };
				auto cellW = [&](const CallCell& aCell)
				{
					return ImGui::CalcTextSize(aCell.Text.c_str()).x + (aCell.Downs > 0 ? lh * 0.75f + ImGui::CalcTextSize(std::to_string(aCell.Downs).c_str()).x + (aCell.Text.empty() ? 0 : 4) : 0.0f);
				};
				std::vector<float> colW(nc, 0.0f), colX(nc, 0.0f);
				for (size_t i = 0; i < nc; i++) { colW[i] = headW(i); }
				for (const CallRow& r : k.Rows) { for (size_t i = 0; i < r.Table.size() && i < nc; i++) { colW[i] = std::max(colW[i], cellW(r.Table[i])); } }
				float cx = aX + 10;
				for (size_t i = 0; i < nc; i++) { colX[i] = cx; cx += colW[i] + lh * 1.1f; }
				auto right = [&](size_t i) { return !k.Rows.empty() && i < k.Rows[0].Table.size() && k.Rows[0].Table[i].Right; };
				for (size_t i = 0; i < nc; i++)
				{
					dl->AddText(ImGui::GetFont(), small, ImVec2(right(i) ? colX[i] + colW[i] - headW(i) : colX[i], y + lh * 0.1f), muted, k.Head[i].c_str());
					if (i < k.HeadTips.size() && !k.HeadTips[i].empty() && ImGui::IsMouseHoveringRect(ImVec2(colX[i], y), ImVec2(colX[i] + colW[i], y + lh))) { TipWrapped(k.HeadTips[i]); }
				}
				y += lh + 2;
				size_t rows = 0;
				for (const CallRow& r : k.Rows)
				{
					if (rows++ >= 40) { continue; }
					if (rows % 2 == 0) { dl->AddRectFilled(ImVec2(aX + 4, y - 1), ImVec2(aX + aW - 4, y + lh + 1), IM_COL32(255, 255, 255, 7)); }
					for (size_t i = 0; i < r.Table.size() && i < nc; i++)
					{
						const CallCell& cell = r.Table[i];
						const ImU32 col = cell.Kind == 0 ? kYou : cell.Kind == 1 ? kMid : cell.Kind == 2 ? kEnemy : cell.Kind == 3 ? muted : ink;
						const float tx = cell.Right ? colX[i] + colW[i] - cellW(cell) : colX[i];
						dl->AddText(ImVec2(tx, y), col, cell.Text.c_str());
						if (cell.Downs > 0)
						{
							// allies downed: the down mark and the count, as on the strip
							const float mx = tx + ImGui::CalcTextSize(cell.Text.c_str()).x + (cell.Text.empty() ? 0 : 4);
							Mark(dl, ImVec2(mx + lh * 0.3f, y + lh * 0.5f), lh * 0.28f, 4, kEnemy);
							dl->AddText(ImVec2(mx + lh * 0.75f, y), ink, std::to_string(cell.Downs).c_str());
						}
						if (!cell.Tip.empty() && ImGui::IsMouseHoveringRect(ImVec2(colX[i] - 4, y), ImVec2(colX[i] + colW[i] + 4, y + lh))) { TipWrapped(cell.Tip); }
					}
					y += lh + 2;
				}
				if (rows > 40) { dl->AddText(ImGui::GetFont(), small, ImVec2(aX + 6, y), muted, ("and " + std::to_string(rows - 40) + " more").c_str()); y += lh; }
				dl->AddRect(ImVec2(aX, top), ImVec2(aX + aW, y + 4), IM_COL32(0x3a, 0x67, 0xa6, 255));
				y += 8;
				return;
			}
			// the skill column only when rows of one part differ in it (a part's one skill is in its heading)
			bool oneSkill = std::all_of(k.Rows.begin(), k.Rows.end(), [&](const CallRow& r)
			{
				auto first = std::find_if(k.Rows.begin(), k.Rows.end(), [&](const CallRow& q) { return q.Part == r.Part; });
				return r.Skill == first->Skill;
			});
			// the name column as wide as its widest entry ("2 wells" needs less than a player's name), the rest for the text
			float whoW = 0;
			for (const CallRow& r : k.Rows) { whoW = std::max(whoW, ImGui::CalcTextSize(r.Who.c_str()).x); }
			const float cTime = aX + 14, cWho = cTime + lh * 3.2f, cSkill = cWho + std::min(lh * 9.5f, whoW + lh * 0.8f), cWhat = cSkill + (oneSkill ? 0 : lh * 11);
			// parts (a Warrior's opener and melee burst) fold: a heading line each, with its count; open, its rows
			std::vector<std::string> parts;
			for (const CallRow& r : k.Rows) { if (!r.Part.empty() && std::find(parts.begin(), parts.end(), r.Part) == parts.end()) { parts.push_back(r.Part); } }
			const bool folding = parts.size() > 1;
			std::string part, section;
			bool partOpen = true;
			size_t shown = 0;
			// a grid card (the wells): a short strip, the enemies downed after the call, then a cell per column
			const bool grid = !k.Columns.empty();
			const float stripX = cWhat + lh * 4.5f;
			const float stripW2 = grid ? lh * 7 : std::min(lh * 16, std::max(lh * 10, aX + aW - stripX - lh * 16));
			const float downsX = stripX + stripW2 + lh * 0.6f, cellsX = downsX + lh * 2.2f, colW = lh * 2.6f;
			if (grid)
			{
				// the columns' names: the first name, cut to the column; the whole name on hover
				for (size_t i = 0; i < k.Columns.size(); i++)
				{
					std::string n = k.Columns[i].substr(0, k.Columns[i].find(' '));
					float cx = cellsX + i * colW;
					dl->PushClipRect(ImVec2(cx, y), ImVec2(cx + colW - 3, y + lh), true);
					dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(cx, y + lh * 0.1f), muted, n.c_str());
					dl->PopClipRect();
					if (ImGui::IsMouseHoveringRect(ImVec2(cx, y), ImVec2(cx + colW - 3, y + lh))) { ImGui::SetTooltip("%s", k.Columns[i].c_str()); }
				}
				y += lh + 2;
			}
			for (const CallRow& r : k.Rows)
			{
				if (r.Part != part)
				{
					part = r.Part;
					section.clear();
					if (!part.empty())
					{
						int good = 0, all = 0;
						for (const CallRow& q : k.Rows) { if (q.Part == part) { all++; good += q.Good; } }
						std::string key = k.Key + "/" + part;
						partOpen = !folding || s.CallPartOpen.count(key) > 0;
						y += 2;
						ImGui::SetCursorScreenPos(ImVec2(aX + 4, y));
						ImGui::PushID(key.c_str());
						bool clicked = folding && ImGui::InvisibleButton("part", ImVec2(aW - 8, lh + 2));
						bool hov = folding && ImGui::IsItemHovered();
						ImGui::PopID();
						if (clicked) { if (partOpen) { s.CallPartOpen.erase(key); } else { s.CallPartOpen.insert(key); } partOpen = !partOpen; }
						if (hov) { dl->AddRectFilled(ImVec2(aX + 4, y), ImVec2(aX + aW - 4, y + lh + 2), IM_COL32(0x1d, 0x26, 0x36, 255)); }
						// the part's rule on its heading's hover
						if (ImGui::IsMouseHoveringRect(ImVec2(aX + 4, y), ImVec2(aX + aW - 4, y + lh + 2)))
						{
							auto tip = k.PartTips.find(part);
							if (tip != k.PartTips.end()) { TipWrapped(tip->second + (folding ? (partOpen ? "\nClick: close" : "\nClick: open") : "")); }
						}
						float hx = aX + 6;
						if (folding) { ImGui::RenderArrow(dl, ImVec2(hx, y + 1), muted, partOpen ? ImGuiDir_Down : ImGuiDir_Right, 0.7f); hx += lh; }
						dl->AddText(ImVec2(hx, y + 1), ink, part.c_str());
						std::string count = std::to_string(good) + " of " + std::to_string(all) + " on the call";
						dl->AddText(ImVec2(hx + ImGui::CalcTextSize(part.c_str()).x + 12, y + 1), good * 4 >= all * 3 ? kYou : good * 2 >= all ? kMid : kEnemy, count.c_str());
						y += lh + 4;
					}
				}
				if (!partOpen) { continue; }
				if (shown++ >= 40) { continue; }
				if (r.Section != section)
				{
					section = r.Section;
					if (!section.empty())
					{
						// a divider with its label: "Wells at 0:23"
						float tw = ImGui::GetFont()->CalcTextSizeA(SmallSize(), FLT_MAX, 0, section.c_str()).x;
						dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(aX + 10, y), kYou, section.c_str());
						dl->AddLine(ImVec2(aX + 16 + tw, y + lh * 0.5f), ImVec2(aX + aW - 8, y + lh * 0.5f), IM_COL32(0x3a, 0x67, 0xa6, 140));
						y += lh + 1;
					}
				}
				const bool strip = !r.Ref.empty();
				if (r.Section != "Missed") { dl->AddText(ImVec2(cTime, y), muted, Duration(r.Ms).c_str()); }
				dl->PushClipRect(ImVec2(cWho, y), ImVec2(cSkill - 6, y + lh), true);
				dl->AddText(ImVec2(cWho, y), ink, r.Who.c_str());
				dl->PopClipRect();
				// a burst's player: their skills around the call, on hover
				if (r.By && ImGui::IsMouseHoveringRect(ImVec2(cWho, y), ImVec2(cSkill - 6, y + lh)))
				{
					std::map<int32_t, std::pair<int, double>> bySkill; // skill -> hits, damage
					double total = 0;
					for (const auto& h : r.By->HitsOut) { if (h.Ms >= r.At - 3000 && h.Ms <= r.At + 4000) { bySkill[h.Skill].first++; bySkill[h.Skill].second += h.Damage; total += h.Damage; } }
					std::vector<std::pair<double, int32_t>> order;
					for (auto& [sk, v] : bySkill) { order.push_back({v.second, sk}); }
					std::sort(order.rbegin(), order.rend());
					ImGui::BeginTooltip();
					ImGui::Text("%s, 3 s before to 4 s after %s: %s to players", r.Who.c_str(), Duration(r.At).c_str(), Num(total).c_str());
					float sx = ImGui::GetCursorPosX();
					ImGui::TextColored(kMuted, "Skill"); ImGui::SameLine(sx + lh * 13); ImGui::TextColored(kMuted, "Casts"); ImGui::SameLine(sx + lh * 16.5f); ImGui::TextColored(kMuted, "Hits");
					ImGui::SameLine(sx + lh * 19.5f); ImGui::TextColored(kMuted, "Damage");
					for (size_t i = 0; i < order.size() && i < 12; i++)
					{
						int32_t sk = order[i].second;
						int casts = 0;
						auto row = r.By->Skills.find(sk);
						if (row != r.By->Skills.end()) { for (int32_t ms : row->second.CastMs) { casts += ms >= r.At - 3000 && ms <= r.At + 4000; } }
						SkillIcon(sk, SkillName(*c.F, sk));
						ImGui::TextUnformatted(SkillName(*c.F, sk).c_str());
						ImGui::SameLine(sx + lh * 13); ImGui::Text("%d", casts);
						ImGui::SameLine(sx + lh * 16.5f); ImGui::Text("%d", bySkill[sk].first);
						ImGui::SameLine(sx + lh * 19.5f); ImGui::TextUnformatted(Num(order[i].first).c_str());
					}
					if (order.empty()) { ImGui::TextColored(kMuted, "No damage to players then."); }
					ImGui::EndTooltip();
				}
				if (!oneSkill)
				{
					dl->PushClipRect(ImVec2(cSkill, y), ImVec2(cWhat - 6, y + lh), true);
					dl->AddText(ImVec2(cSkill, y), muted, r.Skill.c_str());
					dl->PopClipRect();
				}
				if (strip)
				{
					// timed against a moment: the offset, a -3 to +3 s strip with the on-the-call window shaded, the cast (or
					// every cast of a call, the wells), then any note, wrapped
					std::string offText = r.What, note;
					if (size_t cut = offText.find("  "); cut != std::string::npos) { note = offText.substr(cut + 2); offText = offText.substr(0, cut); }
					dl->AddText(ImVec2(cWhat, y), r.Good ? kYou : kEnemy, offText.c_str());
					auto sx = [&](int32_t aMs) { return stripX + stripW2 * (0.5f + std::clamp(aMs / 6000.0f, -0.5f, 0.5f)); };
					dl->AddRectFilled(ImVec2(stripX, y + 1), ImVec2(stripX + stripW2, y + lh - 1), IM_COL32(0x15, 0x17, 0x1b, 255));
					dl->AddRectFilled(ImVec2(sx(r.WinFrom), y + 1), ImVec2(sx(r.WinTo), y + lh - 1), IM_COL32(0x1f, 0x2b, 0x3c, 255));
					for (int32_t ms : {r.WinFrom, r.WinTo}) { dl->AddLine(ImVec2(sx(ms), y + 1), ImVec2(sx(ms), y + lh - 1), IM_COL32(0x3b, 0x42, 0x50, 255)); }
					dl->AddLine(ImVec2(stripX + stripW2 * 0.5f, y + 1), ImVec2(stripX + stripW2 * 0.5f, y + lh - 1), ink, 2.0f);
					if (r.Marks.empty()) { Mark(dl, ImVec2(sx(r.Offset), y + lh * 0.5f), lh * 0.3f, r.Good ? 0 : 4, r.Good ? kYou : kEnemy); }
					for (const CallMark& m : r.Marks) { Mark(dl, ImVec2(sx(m.Off), y + lh * 0.5f), lh * 0.24f, m.Good ? 1 : 4, m.Good ? kYou : kEnemy); }
					// a call's casts on hover: who, which, when
					if (!r.Marks.empty() && ImGui::IsMouseHoveringRect(ImVec2(cWho, y), ImVec2(stripX + stripW2, y + lh)))
					{
						std::vector<const CallMark*> order;
						for (const CallMark& m : r.Marks) { order.push_back(&m); }
						std::sort(order.begin(), order.end(), [](const CallMark* a, const CallMark* b) { return a->Off < b->Off; });
						ImGui::BeginTooltip();
						ImGui::Text("%s at %s, against the middle cast", r.Who.c_str(), Duration(r.Ms).c_str());
						if (ImGui::BeginTable("marks", 3, ImGuiTableFlags_SizingFixedFit))
						{
							for (const CallMark* m : order)
							{
								char off[16];
								std::snprintf(off, sizeof off, "%+.1f s", m->Off / 1000.0);
								ImGui::TableNextRow();
								NumCell(off, m->Good ? nullptr : &kWorseV);
								ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(m->Who.c_str());
								ImGui::TableSetColumnIndex(2); ImGui::TextColored(kMuted, "%s", m->Skill.c_str());
							}
							ImGui::EndTable();
						}
						ImGui::EndTooltip();
					}
					if (grid)
					{
						// the enemies downed after the call, then each column's cell (both wells, side by side)
						if (r.Downs > 0)
						{
							Mark(dl, ImVec2(downsX + lh * 0.3f, y + lh * 0.5f), lh * 0.3f, 3, kYou);
							dl->AddText(ImVec2(downsX + lh * 0.7f, y), ink, std::to_string(r.Downs).c_str());
							if (ImGui::IsMouseHoveringRect(ImVec2(downsX, y), ImVec2(cellsX - 2, y + lh))) { ImGui::SetTooltip("%d %s downed in the 5 s after the call", r.Downs, r.Downs == 1 ? "enemy" : "enemies"); }
						}
						for (size_t i = 0; i < r.Cells.size(); i++) { ChainAt(dl, ImVec2(cellsX + i * colW, y), lh, r.Cells[i], false); }
						note.clear();
					}
					float chainEnd = stripX + stripW2;
					if (!r.Chain.empty())
					{
						// the row's actions in order, as icons (a Warrior's opener); the note after them
						chainEnd = stripX + stripW2 + 10 + ChainAt(dl, ImVec2(stripX + stripW2 + 10, y), lh, r.Chain);
					}
					if (!note.empty())
					{
						// beside the strip (and the chain) when it fits, else on a line of its own under the row, as wide as the pane
						float nx = chainEnd + 10, nw = aX + aW - 6 - nx;
						if (ImGui::CalcTextSize(note.c_str()).x > nw) { y += lh; nx = cWho; nw = aX + aW - 6 - nx; }
						dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(nx, y), r.NoteBad ? kEnemy : muted, note.c_str(), nullptr, nw);
						y += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, nw, note.c_str()).y - lh;
					}
				}
				else
				{
					// the row's skills as icons first (a burst away from any call), then the text, which wraps (a list of missed
					// spikes can be long)
					float tx = cWhat;
					if (!r.Chain.empty()) { tx = cWhat + ChainAt(dl, ImVec2(cWhat, y), lh, r.Chain) + 8; }
					dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, y), r.Good ? ink : kEnemy, r.What.c_str(), nullptr, aX + aW - tx - 8);
					y += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, aX + aW - tx - 8, r.What.c_str()).y - lh;
				}
				y += lh + 2;
			}
			if (shown > 40) { dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(aX + 6, y), muted, ("and " + std::to_string(shown - 40) + " more").c_str()); y += lh; }
			bool anyStrip = false;
			for (const CallRow& r : k.Rows) { anyStrip |= !r.Ref.empty() && (!folding || s.CallPartOpen.count(k.Key + "/" + r.Part) > 0); }
			if (anyStrip)
			{
				dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(stripX, y), muted, "-3 s \xc2\xb7 the call \xc2\xb7 +3 s; on the call: the shaded part");
				y += lh;
			}
			dl->AddRect(ImVec2(aX, top), ImVec2(aX + aW, y + 4), IM_COL32(0x3a, 0x67, 0xa6, 255));
			y += 8;
		};

		// the cards, by when they should happen, as many to a row as fit a long skill name; an opened card's detail
		// comes right after its row
		const int cols = std::clamp(static_cast<int>(width / (lh * 15)), 2, 6);
		const float cgap = 4, cw = (width - cgap * (cols - 1)) / cols, ch = lh * 3.1f;
		for (int g = 0; g < 3; g++)
		{
			std::vector<const CallCard*> cards;
			for (const CallCard& k : calls) { if (k.Group == g) { cards.push_back(&k); } }
			if (cards.empty()) { continue; }
			dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(at.x, y), muted, groups[g]);
			y += lh + 1;
			for (size_t row = 0; row < cards.size(); row += cols)
			{
				const CallCard* opened = nullptr;
				for (size_t i = row; i < cards.size() && i < row + cols; i++)
				{
					const CallCard& k = *cards[i];
					ImVec2 p(at.x + (i - row) * (cw + cgap), y);
					ImGui::SetCursorScreenPos(p);
					ImGui::PushID(k.Key.c_str());
					bool clicked = ImGui::InvisibleButton("call", ImVec2(cw, ch));
					bool hov = ImGui::IsItemHovered();
					ImGui::PopID();
					bool isOpen = s.CallOpen == k.Key;
					if (clicked) { s.CallOpen = isOpen ? std::string() : k.Key; isOpen = !isOpen; }
					if (isOpen) { opened = &k; }
					if (hov) { TipWrapped(k.Name + ": " + k.Should + "." + (k.Detail.empty() ? "" : "\n" + k.Detail) + (isOpen ? "\nClick: close" : "\nClick: who and when")); }
					ImU32 col = colOf(k.Kind);
					dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), hov || isOpen ? IM_COL32(0x1d, 0x26, 0x36, 255) : IM_COL32(0x1b, 0x1d, 0x22, 255));
					dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + ch), col);
					if (isOpen)
					{
						// joined to its detail below: open at the bottom
						dl->AddLine(p, ImVec2(p.x + cw, p.y), kYou);
						dl->AddLine(p, ImVec2(p.x, p.y + ch + cgap), kYou);
						dl->AddLine(ImVec2(p.x + cw, p.y), ImVec2(p.x + cw, p.y + ch + cgap), kYou);
					}
					dl->PushClipRect(p, ImVec2(p.x + cw - 3, p.y + ch), true);
					IconAt(dl, ImVec2(p.x + 7, p.y + 3), lh, k.Skill, k.Name);
					dl->AddText(ImVec2(p.x + lh + 11, p.y + 3), ink, k.Name.c_str());
					Mark(dl, ImVec2(p.x + 7 + lh * 0.35f, p.y + lh * 1.6f + 1), lh * 0.3f, shapeOf(k.Kind), col);
					dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(p.x + 7 + lh * 0.9f, p.y + lh * 1.1f + 3), col, k.Verdict.c_str());
					dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(p.x + 7, p.y + lh * 2.05f + 3), muted, k.Short.c_str());
					dl->PopClipRect();
				}
				y += ch + cgap;
				if (opened) { detail(*opened); }
			}
			y += 2;
		}
		ImGui::SetCursorScreenPos(ImVec2(at.x, y));
		ImGui::Dummy(ImVec2(width, 1));
	}
}
