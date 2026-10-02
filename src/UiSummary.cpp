// Summary tab (the v6 design, style 3b, signed off 2026-09-26): the round for both sides in the same five columns,
// the enemy squad (a click copies it), your down as a chain from cause to outcome, and your job for the next round.
// Every tile is an icon or a number with a word under it; the detail (top 10 skills, each down, why no stability)
// is on hover. Worked out once per round: the tab draws every frame.
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		const ImU32 kTileBg = IM_COL32(0x1b, 0x1d, 0x22, 255);
		const ImU32 kBoxBg = IM_COL32(0x15, 0x17, 0x1b, 255);
		const ImU32 kBoxEdge = IM_COL32(0x2c, 0x30, 0x38, 255);

		// ---- the facts, worked out once per round -----------------------------------------------------------------

		struct SkillSum { int32_t Skill = 0; std::string Name, Who; int64_t Damage = 0; int Hits = 0; };
		struct TopList { std::vector<SkillSum> Top; int64_t Total = 0, Rest = 0; int RestCount = 0; int Enemies = 0; };

		// Why a player had no stability at a moment: never had any, ran out, used up by an earlier CC, stripped or corrupted
		struct StabState
		{
			bool Had = false;
			std::string Word;    // "corrupted 0.4 s before"
			std::string Short;   // "stability corrupted", for a down's line
			std::string Detail;  // hover: by whom and when
			std::string Last;    // hover: who last gave them stability
		};

		struct DownLine { std::string Time, Name, Why, End; int Sg = 0; bool Died = false; };

		struct ChainTile { std::string Value, Word, Extra, Tip; int Icon = 0; int32_t Skill = 0; std::string SkillName; Analysis::CcKind Cc = Analysis::CC_Other; };
		enum ChainIcon { I_None, I_Cc, I_Stab, I_Burst, I_Down, I_Died, I_Revive, I_Skill };

		struct Facts
		{
			// round
			std::string Score, Result, Length;
			ImU32 ResultCol = 0;
			int EnemyDowns = 0, EnemyDeaths = 0, SquadDowns = 0, SquadDeaths = 0;
			SpikeTally Ours;
			std::vector<int64_t> Theirs;
			int OurCc = 0, CcOnUs = 0, CcOnUsBare = 0, OurStrips = 0, TheirStrips = 0;
			Analysis::CcKind CcKind = Analysis::CC_Other;
			int64_t OurDamage = 0, TheirDamage = 0;
			TopList OurTop, TheirTop;
			int MostSg = -1, MostSgDowns = 0;
			std::vector<DownLine> Downs;
			struct Spec { std::string Name; int Count = 0; Player Icon; };
			std::vector<Spec> Enemy;
			int EnemyTotal = 0;
			std::string CopyLine;
			// you
			bool HasYou = false;
			std::string NoYouText;
			bool WentDown = false;
			std::vector<ChainTile> Chain;   // your down, cause to outcome; or your round when you weren't downed
			std::string DeathKey;           // your down in the Deaths tab
			std::string YouLine;            // the summary window: your last down in a line
			TopList YourHits;               // the 6 s before your down (or the round, when not downed)
			std::string YourHitsTitle;
			// next round
			NextRoundFacts Next;
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

		// An enemy's spec as a Player, so SpecIconAt draws its class icon (elite ids as in Evtc's SpecName)
		Player SpecPlayer(const std::string& aSpec)
		{
			struct Row { const char* Name; uint32_t Prof, Elite; };
			static const Row kRows[] = {
				{"Guardian", 1, 0}, {"Dragonhunter", 1, 27}, {"Firebrand", 1, 62}, {"Willbender", 1, 65}, {"Luminary", 1, 81},
				{"Warrior", 2, 0}, {"Berserker", 2, 18}, {"Spellbreaker", 2, 61}, {"Bladesworn", 2, 68}, {"Paragon", 2, 74},
				{"Engineer", 3, 0}, {"Scrapper", 3, 43}, {"Holosmith", 3, 57}, {"Mechanist", 3, 70}, {"Amalgam", 3, 75},
				{"Ranger", 4, 0}, {"Druid", 4, 5}, {"Soulbeast", 4, 55}, {"Untamed", 4, 72}, {"Galeshot", 4, 78},
				{"Thief", 5, 0}, {"Daredevil", 5, 7}, {"Deadeye", 5, 58}, {"Specter", 5, 71}, {"Antiquary", 5, 77},
				{"Elementalist", 6, 0}, {"Tempest", 6, 48}, {"Weaver", 6, 56}, {"Catalyst", 6, 67}, {"Evoker", 6, 80},
				{"Mesmer", 7, 0}, {"Chronomancer", 7, 40}, {"Mirage", 7, 59}, {"Virtuoso", 7, 66}, {"Troubadour", 7, 73},
				{"Necromancer", 8, 0}, {"Reaper", 8, 34}, {"Scourge", 8, 60}, {"Harbinger", 8, 64}, {"Ritualist", 8, 76},
				{"Revenant", 9, 0}, {"Herald", 9, 52}, {"Renegade", 9, 63}, {"Vindicator", 9, 69}, {"Conduit", 9, 79}};
			Player p;
			p.Spec = aSpec;
			for (const Row& r : kRows) { if (aSpec == r.Name) { p.ProfId = r.Prof; p.EliteId = r.Elite; } }
			return p;
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
				s.Word = "never had any";
				s.Short = "no stability given";
				s.Detail = "No stability on you this round before it.";
				return s;
			}
			std::string ago = Secs(aMs - end) + " before";
			for (const auto& st : p.StripsIn)
			{
				if (st.Boon != Analysis::kStability || std::abs(st.Ms - end) > 150) { continue; }
				std::string how = st.Corrupted ? "corrupted" : "stripped";
				s.Word = how + " " + ago;
				s.Short = "stability " + how;
				s.Detail = "Stability " + how + " " + ago + (st.Enemy >= 0 ? " by a " + f.Enemies[st.Enemy].Spec : std::string()) + ".";
				return s;
			}
			for (auto& [ms, kind] : p.StabLost)
			{
				if (kind != 1 || std::abs(ms - end) > 150) { continue; }
				s.Word = "used up " + ago;
				s.Short = "stability used up";
				s.Detail = "An earlier CC took the last stack, " + ago + ".";
				return s;
			}
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
				int32_t last = a.Ms;
				for (const auto& b : p.HitsIn) { if (b.Ms >= a.Ms && b.Ms <= a.Ms + 2000 && b.Ms <= t) { sum += b.Damage - b.Barrier; last = std::max(last, b.Ms); } }
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

		std::string Reviver(const Fight& f, const Down& d, int32_t* aSkill) { return RevivedBy(f, d, aSkill); }

		// A down in a few words: the stability, the CC, else the burst ("stability stripped, pulled")
		std::string ShortWhy(const Fight& f, const Down& d)
		{
			const Player& p = *d.P;
			int32_t t = d.S.From;
			const Player::CcHit* cc = LastCc(p, t - 6000, t);
			StabState st = StabAt(f, p, cc ? cc->Ms : t);
			std::vector<std::string> words;
			if (!st.Had) { words.push_back(st.Short); }
			if (cc) { words.push_back(Analysis::kCcVerbs[cc->Kind]); }
			if (words.size() < 2)
			{
				auto [burst, len] = Burst(p, t);
				if (burst > 0) { words.push_back(Num(burst) + " in " + Secs(len)); }
			}
			std::string out;
			for (size_t i = 0; i < words.size() && i < 3; i++) { out += (i ? ", " : "") + words[i]; }
			return out;
		}

		Facts Work(const Ctx& c)
		{
			const Fight& f = *c.F;
			Facts x;
			// the result
			x.Score = std::to_string(f.EnemyDowns) + " : " + std::to_string(f.SquadDowns);
			x.Result = f.EnemyDowns > f.SquadDowns ? "won" : f.EnemyDowns < f.SquadDowns ? "lost" : "even";
			x.ResultCol = f.EnemyDowns > f.SquadDowns ? kYou : f.EnemyDowns < f.SquadDowns ? kEnemy : kPeer;
			x.Length = Duration(f.DurationMs);
			x.EnemyDowns = f.EnemyDowns; x.EnemyDeaths = f.EnemyDeaths; x.SquadDowns = f.SquadDowns; x.SquadDeaths = f.SquadDeaths;
			// both sides
			x.Ours = OurSpikeTally(c);
			x.Theirs = f.TheirSpikesMs;
			x.OurCc = static_cast<int>(f.OurCcMs.size());
			x.OurStrips = static_cast<int>(f.OurStripsMs.size());
			x.OurDamage = f.SquadDamage;
			x.TheirDamage = f.EnemyDamage;
			std::map<int, int> bareKinds;
			std::vector<const Player::TakenHit*> allIn;
			for (const Player& p : f.Players)
			{
				x.TheirStrips += static_cast<int>(p.StripsIn.size());
				for (const auto& h : p.CcIn) { if (!HadBoonAt(p, Analysis::kStability, h.Ms - 50)) { bareKinds[h.Kind]++; } }
				for (const auto& h : p.HitsIn) { allIn.push_back(&h); }
			}
			for (auto& [g, w] : f.GroupCcWindows) { x.CcOnUs += w; }
			for (auto& [g, k] : f.GroupCcCovered) { x.CcOnUsBare -= k; }
			x.CcOnUsBare += x.CcOnUs;
			int most = 0;
			for (auto& [k, n] : bareKinds) { if (n > most) { most = n; x.CcKind = static_cast<Analysis::CcKind>(k); } }
			x.TheirTop = Top(f, allIn, true);
			// our damage by skill (anything hostile, as the squad's damage total), and who used each
			{
				std::map<std::string, SkillSum> by;
				std::map<std::string, std::map<std::string, std::set<std::string>>> users; // name -> spec -> accounts
				for (const Player& p : f.Players)
				{
					for (auto& [sk, r] : p.Skills)
					{
						if (r.DamageAll <= 0) { continue; }
						std::string name = SkillName(f, sk);
						SkillSum& s = by[name];
						if (s.Damage == 0) { s.Skill = sk; s.Name = name; }
						s.Damage += r.DamageAll;
						s.Hits += r.Hits;
						users[name][p.Spec].insert(p.Account);
						x.OurTop.Total += r.DamageAll;
					}
				}
				for (auto& [name, s] : by)
				{
					int n = 0;
					for (auto& [spec, who] : users[name]) { n += static_cast<int>(who.size()); }
					s.Who = users[name].size() == 1 ? std::to_string(n) + " " + Plural(users[name].begin()->first, n) : std::to_string(n) + " players";
					x.OurTop.Top.push_back(s);
				}
				std::sort(x.OurTop.Top.begin(), x.OurTop.Top.end(), [](const SkillSum& a, const SkillSum& b) { return a.Damage > b.Damage; });
				for (size_t i = 10; i < x.OurTop.Top.size(); i++) { x.OurTop.Rest += x.OurTop.Top[i].Damage; x.OurTop.RestCount++; }
				if (x.OurTop.Top.size() > 10) { x.OurTop.Top.resize(10); }
			}
			// each of our downs in a line, and the subgroup that lost the most
			std::map<int, int> bySg;
			for (const Down& d : Downs(f))
			{
				DownLine l;
				l.Time = Duration(d.S.From);
				l.Name = d.P->Name + (d.P->Pov ? " (you)" : "");
				l.Sg = d.P->Subgroup;
				l.Why = ShortWhy(f, d);
				l.Died = d.Died;
				int32_t skill = 0;
				std::string by = d.Died ? "" : Reviver(f, d, &skill);
				l.End = d.Died ? "died" : by.empty() ? "got up" : "revived";
				if (d.P == c.MeRaw) { x.YouLine = "down at " + l.Time + ": " + l.Why + ", " + (d.Died ? "died" : by.empty() ? "got up" : "revived by " + by); }
				bySg[d.P->Subgroup]++;
				x.Downs.push_back(l);
			}
			for (auto& [g, n] : bySg) { if (n > x.MostSgDowns) { x.MostSgDowns = n; x.MostSg = g; } }
			// the enemy squad by spec, most first
			{
				std::map<std::string, int> specs;
				for (const auto& e : f.Enemies) { if (e.Fought) { specs[e.Spec]++; } }
				for (auto& [name, n] : specs) { x.Enemy.push_back({name, n, SpecPlayer(name)}); }
				std::sort(x.Enemy.begin(), x.Enemy.end(), [](const Facts::Spec& a, const Facts::Spec& b) { return a.Count != b.Count ? a.Count > b.Count : a.Name < b.Name; });
				for (auto& [name, n] : specs) { x.EnemyTotal += n; }
				x.CopyLine = "Enemy " + std::to_string(x.EnemyTotal) + ":";
				for (size_t i = 0; i < x.Enemy.size(); i++) { x.CopyLine += (i ? ", " : " ") + std::to_string(x.Enemy[i].Count) + " " + x.Enemy[i].Name; }
			}
			// you
			x.Next = NextRound(c);
			if (!c.MeRaw) { x.NoYouText = NoYou(f); return x; }
			x.HasYou = true;
			const Player& me = *c.MeRaw;
			std::vector<Down> mine;
			for (const Down& d : Downs(f)) { if (d.P == &me) { mine.push_back(d); } }
			if (!mine.empty())
			{
				x.WentDown = true;
				// the down that killed you, else the last one
				const Down* d = &mine.back();
				for (const Down& e : mine) { if (e.Died) { d = &e; } }
				int32_t t = d->S.From;
				x.DeathKey = f.Stamp + "/" + me.Account + "/" + std::to_string(t);
				const Player::CcHit* cc = LastCc(me, t - 6000, t);
				StabState st = StabAt(f, me, cc ? cc->Ms : t);
				if (cc)
				{
					ChainTile k;
					k.Icon = I_Cc; k.Cc = cc->Kind;
					k.Value = cc->Duration > 0 ? Secs(cc->Duration) : std::string("CC");
					k.Word = std::string(Analysis::kCcVerbs[cc->Kind]) + (st.Had ? ", stability on" : " \xc2\xb7 no stability:");
					if (!st.Had) { k.Extra = st.Word; }
					k.Tip = st.Had ? "You had stability; the CC still landed (" + std::string(Analysis::kCcVerbs[cc->Kind]) + ")." : st.Detail;
					if (!st.Last.empty()) { k.Tip += "\n" + st.Last; }
					x.Chain.push_back(k);
				}
				else if (!st.Had)
				{
					ChainTile k;
					k.Icon = I_Stab; k.Value = "none";
					k.Word = "stability at the down:"; k.Extra = st.Word;
					k.Tip = st.Detail + (st.Last.empty() ? "" : "\n" + st.Last);
					x.Chain.push_back(k);
				}
				// stability that reached you after the CC, before the down was over
				if (cc && !st.Had)
				{
					int idx = static_cast<int>(&me - f.Players.data());
					const Player* giver = nullptr;
					const Player::StabGive* first = nullptr;
					for (const Player& q : f.Players)
					{
						for (const auto& g : q.StabGives)
						{
							if (g.Ms <= cc->Ms || g.Ms > t || std::find(g.Targets.begin(), g.Targets.end(), idx) == g.Targets.end()) { continue; }
							if (!first || g.Ms < first->Ms) { first = &g; giver = &q; }
						}
					}
					if (first)
					{
						ChainTile k;
						k.Icon = I_Stab;
						k.Value = "+" + Secs(first->Ms - cc->Ms);
						k.Word = "stability came after the CC";
						k.Extra = giver == &me ? "your own" : giver->Name;
						k.Tip = (giver == &me ? std::string("You") : giver->Name) + (first->Skill ? " (" + SkillName(f, first->Skill) + ")" : std::string()) + ": " +
							Secs(first->Ms - cc->Ms) + " after the CC, " + Secs(t - first->Ms) + " before your down.";
						x.Chain.push_back(k);
					}
				}
				auto [burst, len] = Burst(me, t);
				std::vector<const Player::TakenHit*> hits;
				for (const auto& h : me.HitsIn) { if (h.Ms >= t - 6000 && h.Ms <= t) { hits.push_back(&h); } }
				x.YourHits = Top(f, hits, true);
				x.YourHitsTitle = "6 s before your down: " + Num(double(x.YourHits.Total)) + " from " + std::to_string(x.YourHits.Enemies) + (x.YourHits.Enemies == 1 ? " enemy" : " enemies");
				if (burst > 0)
				{
					ChainTile k;
					k.Icon = I_Burst; k.Value = Num(burst); k.Word = "to health in " + Secs(len); k.Extra = "hover: top 10";
					x.Chain.push_back(k);
				}
				{
					ChainTile k;
					k.Icon = I_Down; k.Value = Duration(t); k.Word = "down";
					if (mine.size() > 1) { k.Extra = std::to_string(mine.size()) + " downs this round"; }
					x.Chain.push_back(k);
				}
				{
					ChainTile k;
					int32_t skill = 0;
					std::string by = d->Died ? "" : Reviver(f, *d, &skill);
					k.Value = "+" + Secs(d->S.To - d->S.From);
					if (d->Died) { k.Icon = I_Died; k.Word = "died"; }
					else if (!by.empty()) { k.Icon = I_Revive; k.Skill = skill; k.SkillName = skill == 1066 ? "Resurrect" : SkillName(f, skill); k.Word = "revived"; k.Extra = by; }
					else { k.Icon = I_Revive; k.Skill = 1066; k.SkillName = "Resurrect"; k.Word = "got up"; }
					x.Chain.push_back(k);
				}
			}
			else
			{
				// not downed: the CC you took and whether stability was on, the damage you took
				int bare = 0;
				std::map<int, int> kinds;
				std::string tip;
				for (const auto& h : me.CcIn)
				{
					kinds[h.Kind]++;
					StabState st = StabAt(f, me, h.Ms);
					bare += !st.Had;
					tip += (tip.empty() ? "" : "\n") + Duration(h.Ms) + "  " + Analysis::kCcVerbs[h.Kind] + (st.Had ? ", stability on" : ", no stability: " + st.Word);
				}
				ChainTile k;
				k.Icon = me.CcIn.empty() ? I_None : I_Cc;
				int best = 0;
				for (auto& [kd, n] : kinds) { if (n > best) { best = n; k.Cc = static_cast<Analysis::CcKind>(kd); } }
				k.Value = std::to_string(me.CcIn.size());
				k.Word = me.CcIn.empty() ? "no CC on you" : std::string("CC on you, ") + std::to_string(bare) + " with no stability";
				k.Tip = tip;
				x.Chain.push_back(k);
				std::vector<const Player::TakenHit*> hits;
				for (const auto& h : me.HitsIn) { hits.push_back(&h); }
				x.YourHits = Top(f, hits, true);
				x.YourHitsTitle = "Damage you took this round: " + Num(double(x.YourHits.Total));
				ChainTile dmg;
				dmg.Icon = I_Burst; dmg.Value = Num(double(me.DamageTaken)); dmg.Word = "damage taken"; dmg.Extra = "hover: top 10";
				x.Chain.push_back(dmg);
				ChainTile up;
				up.Value = "not downed"; up.Word = "this round";
				x.Chain.push_back(up);
			}
			return x;
		}

		// Worked out once per round; the tab and the summary window can show different rounds, so a few are kept
		const Facts& FactsOf(const Ctx& c)
		{
			static std::map<std::string, std::pair<int, Facts>> cache; // key -> (last use, facts)
			static int clock = 0;
			std::string k = c.F->Stamp + "|" + std::to_string(DataVersion()) + "|" + (c.MeRaw ? c.MeRaw->Account : std::string()) + "|" + S().VsAccount;
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

		using IconFn = std::function<void(ImDrawList*, ImVec2, float)>;

		struct Tile
		{
			std::string Value, Word, Extra;
			IconFn Icon;
			bool Hoverable = false;
			ImU32 Bar = 0;
		};

		float SmallSize() { return ImGui::GetFontSize() * 0.85f; }

		float Wrapped(const std::string& aText, float aWidth)
		{
			if (aText.empty()) { return 0; }
			return ImGui::GetFont()->CalcTextSizeA(SmallSize(), FLT_MAX, aWidth, aText.c_str()).y;
		}

		float TileHeight(const Tile& t, float aWidth)
		{
			return 6 + ImGui::GetTextLineHeight() + 1 + Wrapped(t.Word, aWidth - 12) + Wrapped(t.Extra, aWidth - 12);
		}

		// One tile: an icon and a number, a word under it, and an extra line; returns (hovered, clicked)
		std::pair<bool, bool> DrawTile(const char* aId, const Tile& t, ImVec2 aPos, float aWidth, float aHeight)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			float lh = ImGui::GetTextLineHeight();
			ImGui::SetCursorScreenPos(aPos);
			ImGui::InvisibleButton(aId, ImVec2(aWidth, aHeight));
			bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
			ImVec2 end(aPos.x + aWidth, aPos.y + aHeight);
			dl->AddRectFilled(aPos, end, kTileBg);
			if (t.Bar) { dl->AddRectFilled(aPos, ImVec2(aPos.x + 3, end.y), t.Bar); }
			if (t.Hoverable && hovered) { dl->AddRect(aPos, end, ImGui::GetColorU32(ImGuiCol_Text)); }
			dl->PushClipRect(aPos, end, true);
			float x = aPos.x + 6 + (t.Bar ? 3 : 0), y = aPos.y + 3;
			if (t.Icon) { t.Icon(dl, ImVec2(x, y), lh); x += lh + 5; }
			dl->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_Text), t.Value.c_str());
			float wy = y + lh + 1, wx = aPos.x + 6 + (t.Bar ? 3 : 0), ww = aWidth - 12;
			dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(wx, wy), ImGui::GetColorU32(kMuted), t.Word.c_str(), nullptr, ww);
			if (!t.Extra.empty()) { dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(wx, wy + Wrapped(t.Word, ww)), ImGui::GetColorU32(ImGuiCol_Text), t.Extra.c_str(), nullptr, ww); }
			dl->PopClipRect();
			return {hovered, clicked};
		}

		// A boxed section: its label at the left, a button at the right; aBody draws the rest and returns its height
		bool Section(const char* aLabel, const char* aButton, const std::function<float(ImVec2, float, ImVec2)>& aBody)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			float lh = ImGui::GetTextLineHeight(), pad = 7;
			ImVec2 p0 = ImGui::GetCursorScreenPos();
			float width = ImGui::GetContentRegionAvail().x;
			float labelW = lh * 7.5f;
			float buttonW = ImGui::CalcTextSize("Your down >").x + ImGui::GetStyle().FramePadding.x * 2;
			dl->ChannelsSplit(2);
			dl->ChannelsSetCurrent(1);
			dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(p0.x + pad, p0.y + pad + 2), ImGui::GetColorU32(kMuted), aLabel);
			float h = aBody(ImVec2(p0.x + pad + labelW, p0.y + pad), width - 2 * pad - labelW - buttonW - 10, ImVec2(p0.x + pad, p0.y + pad + lh + 4));
			ImGui::SetCursorScreenPos(ImVec2(p0.x + width - pad - buttonW, p0.y + pad));
			ImGui::PushID(aLabel);
			bool clicked = ImGui::SmallButton(aButton);
			ImGui::PopID();
			float total = std::max(h, lh * 3) + 2 * pad;
			dl->ChannelsSetCurrent(0);
			dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + total), kBoxBg);
			dl->AddRect(p0, ImVec2(p0.x + width, p0.y + total), kBoxEdge);
			dl->ChannelsMerge();
			ImGui::SetCursorScreenPos(p0);
			ImGui::Dummy(ImVec2(width, total));
			ImGui::Spacing();
			return clicked;
		}

		void TopTooltip(const std::string& aTitle, const TopList& t, const char* aWhoHeading)
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(aTitle.c_str());
			if (ImGui::BeginTable("top", 5, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight());
				ImGui::TableSetupColumn("Skill");
				ImGui::TableSetupColumn(aWhoHeading);
				ImGui::TableSetupColumn("Hits");
				ImGui::TableSetupColumn("Damage");
				ImGui::TableNextRow();
				const char* heads[] = {"", "Skill", aWhoHeading, "Hits", "Damage"};
				for (int i = 0; i < 5; i++) { ImGui::TableSetColumnIndex(i); ImGui::TextColored(kMuted, "%s", heads[i]); }
				for (const SkillSum& s : t.Top)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImVec2 p = ImGui::GetCursorScreenPos();
					IconAt(ImGui::GetWindowDrawList(), p, ImGui::GetTextLineHeight(), s.Skill, s.Name);
					ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
					ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(s.Name.c_str());
					ImGui::TableSetColumnIndex(2); ImGui::TextColored(kMuted, "%s", s.Who.c_str());
					NumCell(std::to_string(s.Hits));
					NumCell(Num(double(s.Damage)));
				}
				ImGui::EndTable();
			}
			if (t.RestCount > 0) { ImGui::TextColored(kMuted, "and %s from %d more %s", Num(double(t.Rest)).c_str(), t.RestCount, t.RestCount == 1 ? "skill" : "skills"); }
			ImGui::EndTooltip();
		}

		// Up or down triangle before a number (the font has no arrows)
		IconFn Triangle(bool aUp, ImU32 aColor)
		{
			return [aUp, aColor](ImDrawList* dl, ImVec2 p, float s) { Mark(dl, ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.38f, aUp ? 3 : 4, aColor); };
		}
	}

	std::vector<std::string> SummaryWindowLines(const Ctx& c)
	{
		const Facts& x = FactsOf(c);
		std::vector<std::string> out;
		std::string round = "Allies downed: " + std::to_string(x.SquadDowns);
		if (x.MostSg >= 0 && x.SquadDowns > 1) { round += ", most in sg " + std::to_string(x.MostSg); }
		round += " \xc2\xb7 enemies downed: " + std::to_string(x.EnemyDowns);
		out.push_back(round);
		if (!x.HasYou) { out.push_back(x.NoYouText); return out; }
		out.push_back("You: " + (x.WentDown ? x.YouLine : "not downed"));
		const NextRoundFacts& n = x.Next;
		if (n.Has)
		{
			std::string line = "Next round: " + n.Word + ", " + n.YouText;
			if (n.Usual >= 0) { line += " (usual " + n.UsualText + ")"; }
			out.push_back(line);
			if (n.HasFix) { out.push_back("Fix first: " + n.FixSubject + ", " + n.FixWhat); }
		}
		return out;
	}

	void SummaryTab(const Ctx& c)
	{
		const Facts& x = FactsOf(c);
		State& s = S();
		float lh = ImGui::GetTextLineHeight();
		ImGui::Spacing();

		// ---- the round -------------------------------------------------------------------------------------------
		bool toRound = Section("ROUND", "Round >", [&](ImVec2 at, float width, ImVec2 label) -> float
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			// the result under the label
			dl->AddRectFilled(ImVec2(label.x, label.y), ImVec2(label.x + 3, label.y + lh * 2 + 2), x.ResultCol);
			dl->AddText(ImVec2(label.x + 8, label.y), ImGui::GetColorU32(ImGuiCol_Text), x.Score.c_str());
			dl->AddText(ImGui::GetFont(), SmallSize(), ImVec2(label.x + 8, label.y + lh + 2), ImGui::GetColorU32(kMuted), (x.Result + " \xc2\xb7 " + x.Length).c_str());

			float sideW = ImGui::CalcTextSize("88 players").x + lh + 8;
			float gap = 5, colW = (width - sideW - 4 * gap) / 5;
			auto spikesWord = [&]() -> std::string
			{
				if (x.Theirs.empty()) { return "spikes"; }
				std::string w = Plural("spike", static_cast<int>(x.Theirs.size())) + ", ";
				if (x.Theirs.size() <= 2)
				{
					for (size_t i = 0; i < x.Theirs.size(); i++) { w += (i ? " and " : "") + Duration(x.Theirs[i]); }
				}
				else { w += "first " + Duration(x.Theirs[0]); }
				return w;
			};
			Tile us[5], them[5];
			us[0] = {std::to_string(x.EnemyDowns), "enemies downed, " + std::to_string(x.EnemyDeaths) + " died", "", Triangle(true, kYou)};
			us[1] = {std::to_string(x.Ours.Spikes), x.Ours.Alive ? Plural("spike", x.Ours.Spikes) + ", " + std::to_string(x.Ours.OnTime) + " of " + std::to_string(x.Ours.Alive) + " on time" : "spikes"};
			us[2] = {std::to_string(x.OurCc), "CC on them"};
			us[3] = {std::to_string(x.OurStrips), "boons stripped"};
			us[4] = {Num(double(x.OurDamage)), "damage \xc2\xb7 hover: top 10", "", nullptr, true};
			them[0] = {std::to_string(x.SquadDowns), std::string(x.SquadDowns == 1 ? "ally" : "allies") + " downed, " + std::to_string(x.SquadDeaths) + " died",
				x.MostSg >= 0 && x.SquadDowns > 1 ? "most: sg " + std::to_string(x.MostSg) + " (" + std::to_string(x.MostSgDowns) + ")" : "", Triangle(false, kEnemy), !x.Downs.empty()};
			them[1] = {std::to_string(x.Theirs.size()), spikesWord()};
			Analysis::CcKind kind = x.CcKind;
			them[2] = {std::to_string(x.CcOnUsBare) + " of " + std::to_string(x.CcOnUs), "CC on us, no stability", "",
				[kind](ImDrawList* dl, ImVec2 p, float sz) { CcIconAt(dl, p, sz, kind); }};
			them[3] = {std::to_string(x.TheirStrips), "boons stripped"};
			them[4] = {Num(double(x.TheirDamage)), "damage \xc2\xb7 hover: top 10", "", nullptr, true};
			float hUs = 0, hThem = 0;
			for (int i = 0; i < 5; i++) { hUs = std::max(hUs, TileHeight(us[i], colW)); hThem = std::max(hThem, TileHeight(them[i], colW)); }
			float y = at.y;
			auto side = [&](const char* aName, ImU32 aCol, float aY, float aH)
			{
				dl->AddRectFilled(ImVec2(at.x, aY + aH * 0.5f - 4), ImVec2(at.x + 8, aY + aH * 0.5f + 4), aCol);
				dl->AddText(ImVec2(at.x + 13, aY + (aH - lh) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), aName);
			};
			side("Us", kYou, y, hUs);
			for (int i = 0; i < 5; i++)
			{
				ImGui::PushID(i);
				bool hov = DrawTile("us", us[i], ImVec2(at.x + sideW + i * (colW + gap), y), colW, hUs).first;
				if (i == 4 && hov) { TopTooltip("Our damage: " + Num(double(x.OurTop.Total)) + ", top 10 skills", x.OurTop, "Used by"); }
				ImGui::PopID();
			}
			y += hUs + gap;
			side("Enemy", kEnemy, y, hThem);
			for (int i = 0; i < 5; i++)
			{
				ImGui::PushID(10 + i);
				auto [hov, clk] = DrawTile("them", them[i], ImVec2(at.x + sideW + i * (colW + gap), y), colW, hThem);
				if (i == 0 && hov && !x.Downs.empty())
				{
					ImGui::BeginTooltip();
					ImGui::Text("%d %s downed", x.SquadDowns, x.SquadDowns == 1 ? "ally" : "allies");
					if (ImGui::BeginTable("downs", 5, ImGuiTableFlags_SizingFixedFit))
					{
						for (const DownLine& l : x.Downs)
						{
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0); ImGui::TextColored(kMuted, "%s", l.Time.c_str());
							ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(l.Name.c_str());
							ImGui::TableSetColumnIndex(2); ImGui::TextColored(kMuted, "sg %d", l.Sg);
							ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(l.Why.c_str());
							ImGui::TableSetColumnIndex(4);
							if (l.Died) { ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kEnemy), "%s", l.End.c_str()); }
							else { ImGui::TextUnformatted(l.End.c_str()); }
						}
						ImGui::EndTable();
					}
					ImGui::TextColored(kMuted, "Click: every down in Deaths");
					ImGui::EndTooltip();
				}
				if (i == 0 && clk && !x.Downs.empty()) { s.SwitchTo = T_Deaths; s.DeathFilter = 0; s.DeathKey.clear(); s.DeathSpike = -1; }
				if (i == 4 && hov) { TopTooltip("Their damage on us: " + Num(double(x.TheirTop.Total)) + ", top 10 skills", x.TheirTop, "Enemy"); }
				ImGui::PopID();
			}
			y += hThem + gap + 2;
			// the enemy squad: a click copies it
			{
				static double copiedAt = -10;
				std::string players = std::to_string(x.EnemyTotal) + " players";
				ImGui::SetCursorScreenPos(ImVec2(at.x, y));
				ImVec2 ls = ImGui::CalcTextSize(players.c_str());
				bool clicked = ImGui::InvisibleButton("copy", ImVec2(ls.x + lh, lh));
				bool hov = ImGui::IsItemHovered();
				ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
				dl->AddText(ImVec2(at.x, y), ink, players.c_str());
				dl->AddLine(ImVec2(at.x, y + lh), ImVec2(at.x + ls.x, y + lh), ImGui::GetColorU32(hov ? ImGuiCol_Text : ImGuiCol_TextDisabled));
				// a copy mark: two overlapping squares
				float q = lh * 0.45f, cx = at.x + ls.x + 4, cy = y + lh * 0.2f;
				dl->AddRect(ImVec2(cx + q * 0.4f, cy), ImVec2(cx + q * 1.4f, cy + q), ImGui::GetColorU32(kMuted));
				dl->AddRectFilled(ImVec2(cx, cy + q * 0.4f), ImVec2(cx + q, cy + q * 1.4f), kBoxBg);
				dl->AddRect(ImVec2(cx, cy + q * 0.4f), ImVec2(cx + q, cy + q * 1.4f), ImGui::GetColorU32(kMuted));
				if (clicked) { ImGui::SetClipboardText(x.CopyLine.c_str()); copiedAt = ImGui::GetTime(); }
				if (ImGui::GetTime() - copiedAt < 2.5) { ImGui::SetTooltip("Copied: %s", x.CopyLine.c_str()); }
				else if (hov) { ImGui::SetTooltip("Click: copy the enemy squad as one line"); }
				float sx = at.x + sideW;
				for (size_t i = 0; i < x.Enemy.size(); i++)
				{
					std::string n = std::to_string(x.Enemy[i].Count);
					float w = lh + 4 + ImGui::CalcTextSize(n.c_str()).x + 12;
					if (sx + w > at.x + width) { sx = at.x + sideW; y += lh + 4; }
					SpecIconAt(dl, ImVec2(sx, y), lh, x.Enemy[i].Icon);
					dl->AddText(ImVec2(sx + lh + 4, y), ink, n.c_str());
					if (ImGui::IsMouseHoveringRect(ImVec2(sx, y), ImVec2(sx + w - 8, y + lh))) { ImGui::SetTooltip("%s", Plural(x.Enemy[i].Name, x.Enemy[i].Count).c_str()); }
					sx += w;
				}
				y += lh;
			}
			return std::max(y - at.y, lh * 3.5f);
		});
		if (toRound) { s.SwitchTo = T_Round; s.RoundView = 0; s.SpikeOpen = -1; }

		// ---- you ---------------------------------------------------------------------------------------------------
		bool toDeaths = Section("YOU", x.WentDown ? "Your down >" : "Deaths >", [&](ImVec2 at, float width, ImVec2) -> float
		{
			if (!x.HasYou)
			{
				ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, ImGui::GetColorU32(kMuted), x.NoYouText.c_str(), nullptr, width);
				return lh * 2;
			}
			int n = static_cast<int>(x.Chain.size());
			float arrow = x.WentDown ? 14.0f : 6.0f;
			float tileW = std::min((width - (n - 1) * arrow) / std::max(1, n), lh * 15);
			std::vector<Tile> tiles;
			for (const ChainTile& k : x.Chain)
			{
				Tile t{k.Value, k.Word, k.Extra};
				t.Hoverable = !k.Tip.empty() || k.Icon == I_Burst;
				switch (k.Icon)
				{
				case I_Cc: { Analysis::CcKind kind = k.Cc; t.Icon = [kind](ImDrawList* dl, ImVec2 p, float sz) { CcIconAt(dl, p, sz, kind); }; break; }
				case I_Stab: t.Icon = [](ImDrawList* dl, ImVec2 p, float sz) { IconAt(dl, p, sz, -200 - Analysis::kStability, "Stability", kYou); }; break;
				case I_Burst: t.Icon = [](ImDrawList* dl, ImVec2 p, float sz) { IconAt(dl, p, sz, -303, "Damage burst", kEnemy); }; break;
				case I_Down: t.Icon = [](ImDrawList* dl, ImVec2 p, float sz) { IconAt(dl, p, sz, -301, "Downed", kEnemy); }; break;
				case I_Died: t.Icon = [](ImDrawList* dl, ImVec2 p, float sz)
					{
						float m = sz * 0.2f;
						dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + sz - m, p.y + sz - m), kEnemy, 2.0f);
						dl->AddLine(ImVec2(p.x + sz - m, p.y + m), ImVec2(p.x + m, p.y + sz - m), kEnemy, 2.0f);
					}; break;
				case I_Revive: { int32_t sk = k.Skill; std::string nm = k.SkillName; t.Icon = [sk, nm](ImDrawList* dl, ImVec2 p, float sz) { IconAt(dl, p, sz, sk == 1066 ? -302 : sk, nm, kYou); }; break; }
				default: break;
				}
				tiles.push_back(t);
			}
			float h = 0;
			for (const Tile& t : tiles) { h = std::max(h, TileHeight(t, tileW)); }
			ImDrawList* dl = ImGui::GetWindowDrawList();
			for (int i = 0; i < n; i++)
			{
				float tx = at.x + i * (tileW + arrow);
				ImGui::PushID(100 + i);
				bool hov = DrawTile("chain", tiles[i], ImVec2(tx, at.y), tileW, h).first;
				if (hov && x.Chain[i].Icon == I_Burst) { TopTooltip(x.YourHitsTitle, x.YourHits, "Enemy"); }
				else if (hov && !x.Chain[i].Tip.empty()) { ImGui::SetTooltip("%s", x.Chain[i].Tip.c_str()); }
				ImGui::PopID();
				if (x.WentDown && i + 1 < n) { dl->AddText(ImVec2(tx + tileW + (arrow - ImGui::CalcTextSize(">").x) * 0.5f, at.y + (h - lh) * 0.5f), ImGui::GetColorU32(kMuted), ">"); }
			}
			return h;
		});
		if (toDeaths)
		{
			s.SwitchTo = T_Deaths;
			s.DeathFilter = x.WentDown ? 1 : 0;
			s.DeathKey = x.DeathKey;
			s.DeathSpike = -1;
		}

		// ---- next round ------------------------------------------------------------------------------------------
		bool toYou = Section("NEXT ROUND", "You >", [&](ImVec2 at, float width, ImVec2) -> float
		{
			const NextRoundFacts& nr = x.Next;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			if (!nr.Has)
			{
				dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, ImGui::GetColorU32(kMuted), x.NoYouText.c_str(), nullptr, width);
				return lh * 2;
			}
			// your job's number, against your usual tonight (tick) and the best on your spec this round (the bar's end)
			float jobW = std::min(width * 0.55f, lh * 26);
			std::string ref;
			if (nr.Usual >= 0) { ref = "tick: your usual " + nr.UsualText; }
			if (nr.YouBest) { ref += std::string(ref.empty() ? "" : " \xc2\xb7 ") + "the best on your spec this round"; }
			else if (nr.Best >= 0) { ref += std::string(ref.empty() ? "" : " \xc2\xb7 ") + "end: best " + nr.BestText + " (" + nr.BestWho + ")"; }
			Tile job{nr.YouText, nr.Word, ref, [stab = nr.Stab](ImDrawList* d, ImVec2 p, float sz)
				{
					if (stab) { IconAt(d, p, sz, -200 - Analysis::kStability, "Stability", kYou); }
				}};
			float barH = 8;
			float h = TileHeight(job, jobW) + barH + 4;
			Tile fix;
			if (nr.HasFix)
			{
				fix = {nr.FixSubject, "fix first: " + nr.FixWhat, nr.FixNumbers};
				if (nr.FixSkill != kNoSkill)
				{
					int32_t sk = nr.FixSkill;
					std::string name = nr.FixSubject;
					fix.Icon = [sk, name](ImDrawList* d, ImVec2 p, float sz) { IconAt(d, p, sz, sk, name); };
				}
				fix.Hoverable = true;
				h = std::max(h, TileHeight(fix, width - jobW - 6));
			}
			else { fix = {"none", "no fix stands out", ""}; }
			ImGui::PushID("next");
			DrawTile("job", job, at, jobW, h);
			// the bar: you, your usual as a tick, the best as the end
			if (nr.Known)
			{
				double top = std::max({nr.You, nr.Usual, nr.Best, 1e-9});
				float bx = at.x + 6, bw = jobW - 12, by = at.y + h - barH - 4;
				dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + barH * 0.6f), kTrack);
				dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * float(nr.You / top), by + barH * 0.6f), kYou);
				if (nr.Usual >= 0) { float ux = bx + bw * float(nr.Usual / top); dl->AddLine(ImVec2(ux, by - 3), ImVec2(ux, by + barH), kPeerTick, 2.0f); }
				if (nr.Best >= 0) { float ex = bx + bw * float(nr.Best / top); dl->AddLine(ImVec2(ex, by - 3), ImVec2(ex, by + barH), ImGui::GetColorU32(ImGuiCol_Text), 2.0f); }
			}
			auto [hov, clk] = DrawTile("fix", fix, ImVec2(at.x + jobW + 6, at.y), width - jobW - 6, h);
			if (hov && nr.HasFix) { ImGui::SetTooltip("Click: open it in You"); }
			if (clk && nr.HasFix) { s.SwitchTo = T_You; s.OpenFix = 0; s.YouTonight = false; }
			ImGui::PopID();
			return h;
		});
		if (toYou) { s.SwitchTo = T_You; s.YouTonight = false; }
	}
}
