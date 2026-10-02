// Deaths tab (why did I die, why did xyz die): every down of the round, grouped by the enemy spike that caused it;
// for the one picked, one clock for everything (health, hits, stability, CC, strips, heals, dodges, stability casts)
// and the same events as sentences, in order. Below, the revive skills used against the revive order.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <map>
#include <set>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		constexpr int32_t kBefore = 6000, kAfter = 2000, kLongest = 12000; // the clock: 6 s before, until up or dead (12 s at most)
		constexpr int32_t kBin = 250;                                        // hits are summed per quarter second
		const ImVec4 kWorseV(0xd9 / 255.0f, 0x59 / 255.0f, 0x26 / 255.0f, 1.0f);

		std::string DownKey(const Fight& f, const Down& d) { return f.Stamp + "/" + d.P->Account + "/" + std::to_string(d.S.From); }

		// The clock's end: 2 s after the down, or on to when they got up or died (1 s after), 12 s at most
		int32_t EndOf(const Down& d) { return std::max(d.S.From + kAfter, std::min(d.S.To + 1000, d.S.From + kLongest)); }

		// The Illusion of Life that ran out with this down (they went back down when its 15 s ended), else null. One that
		// ended early with a down was ended by the down: damage while on it (19 of 57 on 22 to 25 Sept), not the cause.
		const Player::Illusion* IllusionEnded(const Down& d)
		{
			for (const auto& il : d.P->IllusionOfLife) { if (il.RanOut && std::abs(il.To - d.S.From) <= 100) { return &il; } }
			return nullptr;
		}

		std::string Rel(int32_t aMs)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%+.1f s", aMs / 1000.0);
			return std::abs(aMs) < 50 ? std::string("0 s") : std::string(buf);
		}

		std::string SkillName(const Fight& f, int32_t aSkill)
		{
			auto it = f.SkillNames.find(aSkill);
			return it == f.SkillNames.end() ? std::to_string(aSkill) : it->second;
		}

		std::string EnemyName(const Fight& f, int aEnemy) { return aEnemy >= 0 && aEnemy < static_cast<int>(f.Enemies.size()) ? "a " + f.Enemies[aEnemy].Spec : std::string("an enemy"); }

		// The enemy strike that came with a CC or strip (same enemy, within 50 ms): its skill, 0 if none
		int32_t SkillWith(const Player& p, int32_t aMs, int aEnemy)
		{
			int32_t best = 0, gap = 51;
			for (const auto& h : p.HitsIn)
			{
				int32_t d = std::abs(h.Ms - aMs);
				if (d < gap && (aEnemy < 0 || h.Enemy == aEnemy)) { gap = d; best = h.Skill; }
			}
			return best;
		}

		// One event on the down's clock, for the rails and the sentences
		struct Event
		{
			int32_t Ms = 0;
			int Rail = 0;              // 0: to them (CC, strips), 1: by or for them (stability, dodges, heals), 2: the burst
			enum Kind { E_Cc, E_Strip, E_Stab, E_Dodge, E_Heal, E_Burst, E_Down, E_Illusion, E_Revive, E_Up, E_Dead } What = E_Cc;
			int32_t Skill = 0;         // the icon: a skill, else Cc / Boon
			Analysis::CcKind Cc = Analysis::CC_Other;
			int Boon = -1;
			bool Good = true;          // a stability cast that reached them
			std::string Text;
		};

		struct Detail
		{
			std::string Verdict;
			std::vector<Event> Events;
			std::string DodgeLine;
			std::string BurstLabel;    // on the burst's box: "24.3k to health in 2.0 s"
			int32_t BurstEnd = 0;      // its last hit (ms)
			// How they went down, in a few steps (the user, 2026-09-30: what killed them, before the second by second):
			// worn down, stability lost, CC, the burst, how it ended. Icon: a skill, else a CC type or a boon struck through.
			struct Step { int32_t Ms = 0; int32_t Skill = 0; int Cc = -1; int Boon = -1; ImU32 Frame = 0; std::string Value, Word, Extra; };
			std::vector<Step> Steps;
			// The enemy skills of the 6 s before the down, most damage first
			struct Top { int32_t Skill = 0; std::string Spec; int Hits = 0; double Damage = 0; };
			std::vector<Top> Tops;
			double TopTotal = 0;
			int TopEnemies = 0;
		};

		// An enemy skill's damage in a window on one player, biggest first: "Arcing Slice 9.8k, Meteor Shower 6.1k"
		std::vector<std::pair<int32_t, double>> SkillsIn(const Player& p, int32_t aFrom, int32_t aTo, bool aHealthOnly)
		{
			std::map<int32_t, double> by;
			for (const auto& h : p.HitsIn) { if (h.Ms >= aFrom && h.Ms <= aTo) { by[h.Skill] += aHealthOnly ? h.Damage - h.Barrier : h.Damage; } }
			std::vector<std::pair<int32_t, double>> out(by.begin(), by.end());
			std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second > b.second; });
			return out;
		}

		std::string TwoSkills(const Fight& f, const std::vector<std::pair<int32_t, double>>& aList)
		{
			std::string s;
			for (size_t i = 0; i < aList.size() && i < 2; i++) { if (aList[i].second > 0) { s += (s.empty() ? "" : ", ") + SkillName(f, aList[i].first) + " " + Num(aList[i].second); } }
			return s;
		}


		Detail Explain(const Fight& f, const Down& d)
		{
			const Player& p = *d.P;
			const int32_t t = d.S.From, from = t - kBefore, to = EndOf(d);
			Detail out;
			auto in = [&](int32_t ms) { return ms >= from && ms <= to; };
			bool stabStripped = false, ccBare = false;
			std::string ccWord;
			for (const auto& h : p.CcIn)
			{
				if (!in(h.Ms)) { continue; }
				Event e;
				e.Ms = h.Ms; e.Rail = 0; e.What = Event::E_Cc; e.Cc = h.Kind;
				bool stab = HadBoonAt(p, Analysis::kStability, h.Ms - 50);
				int32_t sk = SkillWith(p, h.Ms, h.Enemy);
				std::string verb = Analysis::kCcVerbs[h.Kind];
				verb[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(verb[0])));
				e.Text = verb + (h.Duration > 0 ? " " + Num(h.Duration / 1000.0) + " s" : "") + (sk ? " by " + SkillName(f, sk) : "") + (h.Enemy >= 0 ? " (" + f.Enemies[h.Enemy].Spec + ")" : std::string(" (not a player: an NPC or siege)")) +
					(stab ? "." : ", with no stability.");
				if (!stab && h.Ms <= t) { ccBare = true; ccWord = Analysis::kCcVerbs[h.Kind]; }
				out.Events.push_back(e);
			}
			for (const auto& st : p.StripsIn)
			{
				if (!in(st.Ms)) { continue; }
				Event e;
				e.Ms = st.Ms; e.Rail = 0; e.What = Event::E_Strip; e.Boon = st.Boon;
				e.Text = std::string(Analysis::kBoonNames[st.Boon]) + (st.Corrupted ? " corrupted" : " stripped") + " by " + EnemyName(f, st.Enemy) + ".";
				if (st.Boon == Analysis::kStability && st.Ms <= t) { stabStripped = true; }
				out.Events.push_back(e);
			}
			// Stability casts that reached them, or reached their subgroup but not them
			int idx = static_cast<int>(&p - f.Players.data());
			for (const Player& q : f.Players)
			{
				for (const auto& g : q.StabGives)
				{
					if (!in(g.Ms)) { continue; }
					if (g.SelfOnly && &q != &p) { continue; } // the caster's own stability: never theirs to miss
					bool reached = std::find(g.Targets.begin(), g.Targets.end(), idx) != g.Targets.end();
					int group = 0, groupReached = 0;
					for (const Player& m : f.Players)
					{
						if (m.Subgroup != p.Subgroup) { continue; }
						group++;
						groupReached += std::find(g.Targets.begin(), g.Targets.end(), static_cast<int>(&m - f.Players.data())) != g.Targets.end();
					}
					if (!reached && (q.Subgroup != p.Subgroup || groupReached == 0)) { continue; }
					Event e;
					e.Ms = g.Ms; e.Rail = 1; e.What = Event::E_Stab; e.Skill = g.Skill; e.Good = reached;
					std::string by = &q == &p ? "They" : q.Name;
					std::string what = g.Skill ? " cast " + SkillName(f, g.Skill) : std::string(" gave stability (not from a cast: a trait, relic or rune)");
					e.Text = by + what + ": " + (reached ? "it reached " + std::string(&q == &p ? "them" : p.Name) + (g.Ms > t ? ", too late." : ".")
						: "it missed " + p.Name + " (reached " + std::to_string(groupReached) + " of " + std::to_string(group) + " in the subgroup).");
					out.Events.push_back(e);
				}
			}
			// Dodges, and the enemy hits evaded in the 0.75 s after each
			int dodges = 0, evadedAll = 0;
			for (int32_t ms : p.DodgeMs)
			{
				if (!in(ms)) { continue; }
				int evaded = 0;
				for (const auto& h : p.EvadedIn) { evaded += h.Ms >= ms && h.Ms <= ms + 750; }
				if (ms <= t) { dodges++; evadedAll += evaded; } // the line below is about the 6 s before
				Event e;
				e.Ms = ms; e.Rail = 1; e.What = Event::E_Dodge;
				e.Text = evaded ? "Dodged: evaded " + std::to_string(evaded) + (evaded == 1 ? " hit." : " hits.") : "Dodged: nothing evaded.";
				out.Events.push_back(e);
			}
			int32_t lastBefore = -1;
			for (int32_t ms : p.DodgeMs) { if (ms <= t && ms >= from) { lastBefore = ms; } }
			out.DodgeLine = dodges == 0 ? "No dodge in the 6 s before the down." :
				std::to_string(dodges) + (dodges == 1 ? " dodge" : " dodges") + " in the 6 s before, the last at " + Rel(lastBefore - t) + "; " + std::to_string(evadedAll) +
				(evadedAll == 1 ? " hit evaded." : " hits evaded.");
			// Heals, a line per second that had any
			std::map<int32_t, int64_t> heals;
			for (auto& [ms, a] : p.HealsIn) { if (in(ms)) { heals[(ms - from) / 1000] += a; } }
			for (auto& [sec, a] : heals)
			{
				Event e;
				e.Ms = from + sec * 1000 + 500; e.Rail = 1; e.What = Event::E_Heal;
				e.Text = "Healed " + Num(double(a)) + " in this second.";
				out.Events.push_back(e);
			}
			// The burst: the most health damage in 2 s before the down (barrier took the rest), who did it, how fast from 90%
			double total = 0, barrier = 0, enemies = 0;
			std::set<int> hitters;
			std::map<std::string, double> bySpec;
			for (const auto& h : p.HitsIn)
			{
				if (h.Ms < from || h.Ms > t) { continue; }
				total += h.Damage;
				barrier += h.Barrier;
				if (h.Enemy >= 0) { hitters.insert(h.Enemy); bySpec[f.Enemies[h.Enemy].Spec] += h.Damage; }
			}
			enemies = static_cast<double>(hitters.size());
			double best = 0, bestBarrier = 0;
			int32_t bestAt = t;
			for (const auto& a : p.HitsIn)
			{
				if (a.Ms < from || a.Ms > t) { continue; }
				double sum = 0, shield = 0;
				for (const auto& b : p.HitsIn) { if (b.Ms >= a.Ms && b.Ms <= a.Ms + 2000 && b.Ms <= t) { sum += b.Damage - b.Barrier; shield += b.Barrier; } }
				if (sum > best) { best = sum; bestBarrier = shield; bestAt = a.Ms; }
			}
			// The burst from where it really starts: the last hit from which 90% of it is still to come (the window began at
			// its first small hit: "23.1k in 1.99 s" when nearly all of it came in the last 0.2 s; the user, 2026-09-30)
			int32_t burstEnd = bestAt;
			if (best > 0)
			{
				std::vector<std::pair<int32_t, double>> burstHits;
				for (const auto& b : p.HitsIn) { if (b.Ms >= bestAt && b.Ms <= bestAt + 2000 && b.Ms <= t) { burstHits.push_back({b.Ms, double(b.Damage - b.Barrier)}); } }
				std::sort(burstHits.begin(), burstHits.end());
				double rest = best;
				for (auto& [ms, dmg] : burstHits)
				{
					if (rest - dmg < 0.9 * best) { bestAt = ms; break; }
					rest -= dmg;
				}
				for (auto& [ms, dmg] : burstHits) { if (dmg > 0) { burstEnd = ms; } }
				best = 0;
				for (auto& [ms, dmg] : burstHits) { if (ms >= bestAt) { best += dmg; } }
			}
			int32_t full = -1;
			for (auto& [ms, hp] : p.Hp) { if (ms > t) { break; } if (hp >= 9000) { full = ms; } }
			std::string topSpec;
			for (auto& [sp, v] : bySpec) { if (topSpec.empty() || v > bySpec[topSpec]) { topSpec = sp; } }
			if (best > 0)
			{
				Event e;
				e.Ms = bestAt; e.Rail = 2; e.What = Event::E_Burst;
				out.BurstLabel = Num(best) + " to health in " + Num(std::max(100, burstEnd - bestAt) / 1000.0) + " s";
				out.BurstEnd = burstEnd;
				e.Text = Num(best) + " damage to health from here to " + Rel(burstEnd - t) + (bestBarrier > 0 ? " (and " + Num(bestBarrier) + " into barrier)" : "") +
					(topSpec.empty() ? "" : "; most of the 6 s from " + topSpec + "s (" + std::to_string(int(100 * bySpec[topSpec] / std::max(1.0, total) + 0.5)) + "%)") +
					(full >= 0 && t - full <= 6000 ? "; from 90% health to down in " + Num((t - full) / 1000.0) + " s." : ".");
				out.Events.push_back(e);
			}
			// Illusion of Life running out: the reason for the down when it ended with it
			const Player::Illusion* il = IllusionEnded(d);
			if (il)
			{
				Event e;
				e.Ms = t; e.Rail = 1; e.What = Event::E_Illusion; e.Skill = 10244;
				std::string by = il->By >= 0 ? f.Players[il->By].Name + "'s " : std::string();
				e.Text = by + "Illusion of Life " + (il->RanOut ? "ran out" : "ended") + " (cast at " + Duration(il->From) + "): back to downed.";
				out.Events.push_back(e);
			}
			// Revives: plain revives by others and revive skills that went off while they were down
			int32_t revSkill = 0;
			const std::string revBy = d.Died ? std::string() : RevivedBy(f, d, &revSkill);
			for (const Player& q : f.Players)
			{
				if (&q == &p) { continue; }
				for (const auto& v : q.Reviving)
				{
					if (&f.Players[v.Target] != &p || v.To < d.S.From || v.From > d.S.To || v.To - v.From < 500) { continue; } // brief touches say nothing
					Event e;
					e.Ms = std::max(v.From, d.S.From); e.Rail = 1; e.What = Event::E_Revive; e.Skill = 1066;
					e.Text = q.Name + " revived them for " + Num((std::min(v.To, d.S.To) - e.Ms) / 1000.0) + " s.";
					out.Events.push_back(e);
				}
				// every revive skill near them while they were down, also the ones that didn't get them up (the user,
				// 2026-10-01); a Spirit of Nature revives about 0.9 s after its cast, so one just before the down counts
				for (const auto& u : q.ReviveUses)
				{
					const int32_t lag = u.Skill == 12569 ? 1500 : 0;
					if (u.Ms + lag < d.S.From || u.Ms > d.S.To + 100) { continue; }
					const Player::Point* a = NearestPos(p, u.Ms);
					const Player::Point* b = NearestPos(q, u.Ms);
					if (!a || !b || std::hypot(a->X - b->X, a->Y - b->Y) > 1500) { continue; }
					Event e;
					e.Ms = u.Ms; e.Rail = 1; e.What = Event::E_Revive; e.Skill = u.Skill;
					bool upByIt = revSkill == u.Skill && revBy.rfind(q.Name + ",", 0) == 0;
					e.Good = upByIt;
					if (!u.Done)
					{
						const char* why = u.Stop == 8 ? "interrupted" : u.Stop == 9 ? "the caster died" : u.Stop == 10 ? "the caster was downed" : u.Stop == 11 ? "the caster was crowd controlled" : "cancelled";
						e.Text = q.Name + " started " + SkillName(f, u.Skill) + ", but it was cut short (" + why + ").";
					}
					else if (upByIt) { e.Text = q.Name + " used " + SkillName(f, u.Skill) + ": it got them up."; }
					else
					{
						e.Text = q.Name + " used " + SkillName(f, u.Skill) + (u.Ms < d.S.From ? " just before the down" : "") + ": it didn't get them up" +
							(d.Died ? "; they died." : revBy.empty() ? "." : " (" + revBy + " did).");
					}
					out.Events.push_back(e);
				}
			}
			if (!d.Died && d.S.To <= to)
			{
				Event e;
				e.Ms = d.S.To; e.Rail = 1; e.What = Event::E_Up;
				e.Text = "Up again.";
				out.Events.push_back(e);
			}
			if (d.Died && d.S.To <= to)
			{
				Event e;
				e.Ms = d.S.To; e.Rail = 2; e.What = Event::E_Dead;
				e.Text = "Died.";
				out.Events.push_back(e);
			}
			// The down itself: got up or died, revived by whom
			{
				Event e;
				e.Ms = t; e.Rail = 2; e.What = Event::E_Down;
				int32_t ms = d.S.To - d.S.From;
				// how they got up: revived by whom, a rally, or unknown (the user, 2026-10-01: "it just says they got up")
				e.Text = std::string("Down. ") + (d.Died ? "Died after " + Num(ms / 1000.0) + " s." : (ms < 500 ? "Got up at once: " : "Got up after " + Num(ms / 1000.0) + " s: ") + GotUpHow(f, d) + ".");
				out.Events.push_back(e);
			}
			std::stable_sort(out.Events.begin(), out.Events.end(), [](const Event& a, const Event& b) { return a.Ms < b.Ms; });
			// The verdict: who, when, how much from how many, and what went before
			std::string after;
			if (stabStripped && ccBare) { after = ", after their stability was stripped and they were " + ccWord + " with none on"; }
			else if (stabStripped) { after = ", after their stability was stripped"; }
			else if (ccBare) { after = ", after they were " + ccWord + " with no stability"; }
			std::string hit = Num(total) + " damage" + (barrier > 0 ? " (" + Num(barrier) + " into barrier)" : "") +
				(enemies == 0 ? std::string(" from NPCs or siege, no enemy player") : " from " + std::to_string(int(enemies)) + (enemies == 1 ? " enemy" : " enemies")) + " in 6 s";
			if (il)
			{
				out.Verdict = p.Name + (p.Pov ? " (you)" : "") + " went back down at " + Duration(t) + " when " + (il->By >= 0 ? f.Players[il->By].Name + "'s " : std::string()) +
					"Illusion of Life " + (il->RanOut ? "ran out" : "ended") + ", not from damage (" + hit + ").";
			}
			else { out.Verdict = p.Name + (p.Pov ? " (you)" : "") + " went down at " + Duration(t) + ": " + hit + after + "."; }

			// How they went down, step by step
			using Step = Detail::Step;
			if (best > 0 && !il)
			{
				// worn down: the health damage before the burst, when it was a fifth of the 6 s or more
				double before = 0;
				for (const auto& h : p.HitsIn) { if (h.Ms >= from && h.Ms < bestAt) { before += h.Damage - h.Barrier; } }
				if (before >= 0.2 * (before + best))
				{
					Step s;
					s.Ms = from;
					auto list = SkillsIn(p, from, bestAt - 1, true);
					s.Skill = list.empty() ? 0 : list[0].first;
					s.Frame = kEnemy;
					// damage, not health: ArcDPS writes health only now and then, so the bar at the burst's start is often stale
					s.Value = Num(before);
					s.Word = "worn down, " + Rel(from - t) + " to " + Rel(bestAt - t);
					s.Extra = TwoSkills(f, list);
					out.Steps.push_back(s);
				}
			}
			// stability: the last time it was taken in the 6 s, else none on at the down
			{
				const Player::StripHit* last = nullptr;
				for (const auto& st : p.StripsIn) { if (st.Boon == Analysis::kStability && st.Ms >= from && st.Ms <= t) { last = &st; } }
				if (last)
				{
					Step s;
					s.Ms = last->Ms; s.Boon = Analysis::kStability;
					s.Value = Rel(last->Ms - t);
					s.Word = std::string("stability ") + (last->Corrupted ? "corrupted" : "stripped");
					int32_t sk = SkillWith(p, last->Ms, last->Enemy);
					s.Extra = (last->Enemy >= 0 ? f.Enemies[last->Enemy].Spec : std::string("an enemy")) + (sk ? ", " + SkillName(f, sk) : std::string());
					out.Steps.push_back(s);
				}
				else if (!HadBoonAt(p, Analysis::kStability, t - 200))
				{
					Step s;
					s.Ms = t - 1; s.Skill = -200 - Analysis::kStability; s.Frame = kPeer;
					s.Value = "none";
					s.Word = "no stability at the down";
					out.Steps.push_back(s);
				}
			}
			// the last CC in the 3 s before
			{
				const Player::CcHit* last = nullptr;
				for (const auto& h : p.CcIn) { if (h.Ms >= t - 3000 && h.Ms <= t) { last = &h; } }
				if (last)
				{
					Step s;
					s.Ms = last->Ms; s.Cc = last->Kind;
					s.Value = Rel(last->Ms - t);
					s.Word = std::string(Analysis::kCcVerbs[last->Kind]) + (HadBoonAt(p, Analysis::kStability, last->Ms - 50) ? ", stability on" : ", no stability");
					int32_t sk = SkillWith(p, last->Ms, last->Enemy);
					s.Extra = (last->Enemy >= 0 ? f.Enemies[last->Enemy].Spec : std::string("an NPC or siege")) + (sk ? ", " + SkillName(f, sk) : std::string());
					out.Steps.push_back(s);
				}
			}
			if (best > 0)
			{
				Step s;
				s.Ms = bestAt; s.Skill = -303; s.Frame = kEnemy;
				s.Value = Num(best);
				s.Word = "burst in " + Num(std::max(100, burstEnd - bestAt) / 1000.0) + " s, to health";
				s.Extra = TwoSkills(f, SkillsIn(p, bestAt, burstEnd, true));
				out.Steps.push_back(s);
			}
			std::stable_sort(out.Steps.begin(), out.Steps.end(), [](const Step& a, const Step& b) { return a.Ms < b.Ms; });
			{
				Step s;
				s.Ms = t;
				int32_t ms = d.S.To - d.S.From;
				if (d.Died) { s.Skill = -301; s.Frame = kEnemy; s.Value = "+" + Num(ms / 1000.0) + " s"; s.Word = "died"; }
				else
				{
					int32_t skill = 0;
					std::string by = RevivedBy(f, d, &skill);
					bool rally = by.empty() && Rallied(f, d);
					s.Skill = -302; s.Frame = kYou; s.Value = "+" + Num(ms / 1000.0) + " s"; s.Word = !by.empty() ? "revived" : rally ? "rallied" : "got up";
					s.Extra = !by.empty() ? by : rally ? "an enemy died" : "how is unknown"; // "Tam Vey, Illusion of Life": RevivedBy names the skill
				}
				out.Steps.push_back(s);
			}
			// The top enemy skills of the 6 s
			{
				std::map<int32_t, Detail::Top> by;
				std::map<int32_t, std::map<std::string, double>> specs;
				std::set<int> hitters2;
				for (const auto& h : p.HitsIn)
				{
					if (h.Ms < from || h.Ms > t) { continue; }
					auto& x = by[h.Skill];
					x.Skill = h.Skill; x.Hits++; x.Damage += h.Damage;
					out.TopTotal += h.Damage;
					if (h.Enemy >= 0) { specs[h.Skill][f.Enemies[h.Enemy].Spec] += h.Damage; hitters2.insert(h.Enemy); }
				}
				out.TopEnemies = static_cast<int>(hitters2.size());
				for (auto& [sk, x] : by)
				{
					double most = 0;
					for (auto& [sp, v] : specs[sk]) { if (v > most) { most = v; x.Spec = sp; } }
					if (x.Spec.empty()) { x.Spec = "NPC or siege"; }
					out.Tops.push_back(x);
				}
				std::sort(out.Tops.begin(), out.Tops.end(), [](const Detail::Top& a, const Detail::Top& b) { return a.Damage > b.Damage; });
			}
			return out;
		}

		// The steps as tiles in a row, joined by arrows: an icon and the value, a muted word, a line in ink
		void Steps(const Fight& f, const Detail& det)
		{
			if (det.Steps.empty()) { return; }
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float lh = ImGui::GetTextLineHeight(), small = ImGui::GetFontSize() * 0.85f, arrow = 14, pad = 6;
			const float avail = ImGui::GetContentRegionAvail().x;
			const int n = static_cast<int>(det.Steps.size());
			const float w = std::min(200.0f, (avail - arrow * (n - 1)) / n);
			// the tallest tile sets the row: the value line, then the wrapped word and extra
			auto wrapped = [&](const std::string& s) { return s.empty() ? 0.0f : ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, w - pad * 2, s.c_str()).y; };
			float h = 0;
			for (const auto& s : det.Steps) { h = std::max(h, pad + lh + 2 + wrapped(s.Word) + wrapped(s.Extra) + pad); }
			ImVec2 o = ImGui::GetCursorScreenPos();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			for (int i = 0; i < n; i++)
			{
				const auto& s = det.Steps[i];
				ImVec2 p(o.x + i * (w + arrow), o.y);
				dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(0x1b, 0x1d, 0x22, 255));
				ImVec2 ic(p.x + pad, p.y + pad);
				if (s.Cc >= 0) { CcIconAt(dl, ic, lh, static_cast<Analysis::CcKind>(s.Cc)); }
				else if (s.Boon >= 0) { BoonIconAt(dl, ic, lh, s.Boon, true); }
				else if (s.Skill) { IconAt(dl, ic, lh, s.Skill, s.Skill > 0 ? SkillName(f, s.Skill) : s.Word, s.Frame); }
				dl->AddText(ImVec2(ic.x + lh + 6, ic.y), ink, s.Value.c_str());
				float y = ic.y + lh + 2;
				dl->AddText(ImGui::GetFont(), small, ImVec2(p.x + pad, y), muted, s.Word.c_str(), nullptr, w - pad * 2);
				y += wrapped(s.Word);
				dl->AddText(ImGui::GetFont(), small, ImVec2(p.x + pad, y), ink, s.Extra.c_str(), nullptr, w - pad * 2);
				if (i + 1 < n) { dl->AddText(ImVec2(p.x + w + 3, p.y + h * 0.5f - lh * 0.5f), muted, ">"); }
			}
			ImGui::Dummy(ImVec2(avail, h));
		}

		// The top enemy skills of the 6 s: icon, skill, the enemy spec that used it most, hits, damage, share as a bar
		void TopSkills(const Fight& f, const Detail& det)
		{
			if (det.Tops.empty()) { return; }
			ImGui::TextColored(kMuted, "Top skills, 6 s before the down");
			ImGui::SameLine(0, 12);
			ImGui::TextColored(kMuted, "%s from %d %s; bar: share of it", Num(det.TopTotal).c_str(), det.TopEnemies, det.TopEnemies == 1 ? "enemy" : "enemies");
			if (!ImGui::BeginTable("topskills", 5, ImGuiTableFlags_SizingFixedFit)) { return; }
			ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthFixed, 210);
			ImGui::TableSetupColumn("Enemy", ImGuiTableColumnFlags_WidthFixed, 120);
			ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 40);
			ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 60);
			ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthStretch);
			Headers({{"Skill", nullptr}, {"Enemy", "Who used it most"}, {"Hits", nullptr}, {"Damage", "Health and barrier"}, {"Share", "Of the 6 s"}});
			double rest = 0;
			for (size_t i = 0; i < det.Tops.size(); i++)
			{
				const auto& x = det.Tops[i];
				if (i >= 6) { rest += x.Damage; continue; }
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				std::string name = SkillName(f, x.Skill);
				SkillIcon(x.Skill, name);
				ImGui::TextUnformatted(name.c_str());
				Cell(x.Spec, &kMuted);
				NumCell(std::to_string(x.Hits));
				NumCell(Num(x.Damage));
				ImGui::TableNextColumn();
				ImVec2 p = ImGui::GetCursorScreenPos();
				float bw = static_cast<float>(x.Damage / std::max(1.0, det.TopTotal)) * ImGui::GetContentRegionAvail().x;
				Rect(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + ImGui::GetTextLineHeight() * 0.3f), std::max(2.0f, bw), ImGui::GetTextLineHeight() * 0.4f, kEnemy);
				ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight()));
			}
			ImGui::EndTable();
			if (rest > 0) { ImGui::TextColored(kMuted, "and %s from %d more skills", Num(rest).c_str(), static_cast<int>(det.Tops.size()) - 6); }
		}

		void EventIcon(ImDrawList* dl, ImVec2 p, float aSize, const Fight& f, const Event& e)
		{
			switch (e.What)
			{
			case Event::E_Cc: CcIconAt(dl, p, aSize, e.Cc); break;
			case Event::E_Strip: BoonIconAt(dl, p, aSize, e.Boon, true); break;
			case Event::E_Stab: // no cast behind it (a trait, relic or rune): the Stability icon
				if (e.Skill == 0) { IconAt(dl, p, aSize, -200 - Analysis::kStability, "Stability", e.Good ? kYou : kPeer); }
				else { IconAt(dl, p, aSize, e.Skill, SkillName(f, e.Skill), e.Good ? kYou : kPeer); }
				break;
			case Event::E_Dodge: IconAt(dl, p, aSize, 23275, "Dodge", kYou); break;
			case Event::E_Heal: IconAt(dl, p, aSize, -300, "Healing", kYou); break;
			case Event::E_Burst: IconAt(dl, p, aSize, -303, "Damage burst", kEnemy); break;
			case Event::E_Down: IconAt(dl, p, aSize, -301, "Downed", kEnemy); break;
			case Event::E_Illusion: IconAt(dl, p, aSize, e.Skill, "Illusion of Life", kPeer); break;
			case Event::E_Revive: IconAt(dl, p, aSize, e.Skill == 1066 ? -302 : e.Skill, e.Skill == 1066 ? std::string("Resurrect") : SkillName(f, e.Skill), e.Good ? kYou : kPeer); break; // grey: it didn't get them up
			case Event::E_Up: IconAt(dl, p, aSize, -302, "Resurrect", kYou); break;
			case Event::E_Dead:
				dl->AddLine(ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + aSize - 3, p.y + aSize - 3), kEnemy, 2);
				dl->AddLine(ImVec2(p.x + 3, p.y + aSize - 3), ImVec2(p.x + aSize - 3, p.y + 3), kEnemy, 2);
				break;
			}
		}

		// Which lane an event sits in: 0 CC, 1 boons lost, 2 stability; -1: on the health line (heals, revives, the down,
		// up again, Illusion of Life), -2: the burst (a box on the graph), -3: dodges (small, along the graph's top edge:
		// the user, 2026-09-25, a lane of their own wasn't needed)
		int LaneOf(const Event& e)
		{
			switch (e.What)
			{
			case Event::E_Cc: return 0;
			case Event::E_Strip: return 1;
			case Event::E_Stab: return 2;
			case Event::E_Dodge: return -3;
			case Event::E_Burst: return -2;
			default: return -1;
			}
		}

		// A dashed vertical line
		void Dashed(ImDrawList* dl, float aX, float aTop, float aBottom, ImU32 aColor)
		{
			for (float y = aTop; y < aBottom; y += 7) { dl->AddLine(ImVec2(aX, y), ImVec2(aX, std::min(y + 4, aBottom)), aColor, 1.0f); }
		}

		// The clock (the user's v5, 2026-09-25): a lane per kind above the graph (CC, boons lost, stability, dodges), so
		// nothing hides another kind; events at one moment sit side by side from its tick, and what doesn't fit before
		// the next moment becomes "+n" after them. Heals and revives sit on the health line; the burst is a dashed box.
		void DownClock(const Fight& f, const Down& d, const Detail& det)
		{
			const Player& p = *d.P;
			const int32_t t = d.S.From, from = t - kBefore, to = EndOf(d);
			float lh = ImGui::GetTextLineHeight();
			const int kLanes = 3;
			const char* laneNames[kLanes] = {"CC", "Boons lost", "Stability"};
			const float labelW = 90, rail = lh + 8, mainH = lh * 11;
			float width = ImGui::GetContentRegionAvail().x - labelW;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 o = ImGui::GetCursorScreenPos();
			ImVec2 g(o.x + labelW, o.y);
			auto x = [&](double ms) { return g.x + static_cast<float>(std::clamp((ms - from) / double(to - from), 0.0, 1.0)) * width; };
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			float mainY = g.y + rail * kLanes + 4;
			ImGui::SetCursorScreenPos(g);
			ImGui::InvisibleButton("clock", ImVec2(width, mainY - g.y + mainH + lh));
			bool hovered = ImGui::IsItemHovered();
			for (int r = 0; r < kLanes; r++)
			{
				dl->AddText(ImVec2(o.x, g.y + rail * r + 4), muted, laneNames[r]);
				dl->AddRectFilled(ImVec2(g.x, g.y + rail * r), ImVec2(g.x + width, g.y + rail * (r + 1) - 2), IM_COL32(0x15, 0x17, 0x1b, 255));
			}
			dl->AddRectFilled(ImVec2(g.x, mainY), ImVec2(g.x + width, mainY + mainH), kLaneBg);
			// stability on them: a band over the main area
			for (auto& [a, b] : p.BoonOn[Analysis::kStability])
			{
				if (b < from || a > to) { continue; }
				dl->AddRectFilled(ImVec2(x(a), mainY), ImVec2(x(b), mainY + mainH), IM_COL32(57, 135, 229, 50));
			}
			// downed: the time after the down shaded, up to when they got up or died
			dl->AddRectFilled(ImVec2(x(t), mainY), ImVec2(x(std::min(d.S.To, to)), mainY + mainH), IM_COL32(0x3b, 0x3f, 0x46, 110));
			// the burst: a dashed box over its 2 s with the number on it
			for (const Event& e : det.Events)
			{
				if (e.What != Event::E_Burst) { continue; }
				float a = x(e.Ms), b = x(std::max(det.BurstEnd, e.Ms + 100));
				dl->AddRectFilled(ImVec2(a, mainY), ImVec2(b, mainY + mainH), IM_COL32(217, 89, 38, 26));
				Dashed(dl, a, mainY, mainY + mainH, kEnemy);
				Dashed(dl, b, mainY, mainY + mainH, kEnemy);
				std::string label = det.BurstLabel;
				float tw = ImGui::CalcTextSize(label.c_str()).x;
				float lx = std::clamp((a + b - tw) * 0.5f, g.x + 2, g.x + width - tw - 2);
				dl->AddRectFilled(ImVec2(lx - 3, mainY + mainH * 0.35f), ImVec2(lx + tw + 3, mainY + mainH * 0.35f + lh + 2), IM_COL32(16, 16, 17, 230));
				dl->AddText(ImVec2(lx, mainY + mainH * 0.35f + 1), ink, label.c_str());
			}
			// hits summed per quarter second from the bottom: health damage solid, barrier lighter on top; the scale is
			// the biggest quarter second before the down (hits on the downed body are taller and cut at the top)
			{
				std::map<int32_t, std::pair<double, double>> bins; // bin -> (health, barrier)
				for (const auto& h : p.HitsIn)
				{
					if (h.Ms < from || h.Ms > to) { continue; }
					auto& b2 = bins[(h.Ms - from) / kBin];
					b2.first += h.Damage - h.Barrier;
					b2.second += h.Barrier;
				}
				double mx = 1;
				for (auto& [b2, v] : bins) { if (from + b2 * kBin < t) { mx = std::max(mx, v.first + v.second); } }
				float bw = std::max(2.0f, width * kBin / float(to - from) - 2);
				for (auto& [b2, v] : bins)
				{
					float bx = x(from + b2 * kBin) + 1;
					float hh = std::min(mainH * 0.55f, static_cast<float>(v.first / mx) * mainH * 0.55f);
					float hb = std::min(mainH * 0.55f - hh, static_cast<float>(v.second / mx) * mainH * 0.55f);
					if (hh > 0) { dl->AddRectFilled(ImVec2(bx, mainY + mainH - hh), ImVec2(bx + bw, mainY + mainH), kEnemy); }
					if (hb > 0) { dl->AddRectFilled(ImVec2(bx, mainY + mainH - hh - hb), ImVec2(bx + bw, mainY + mainH - hh), IM_COL32(217, 89, 38, 110)); }
				}
				for (const auto& h : p.EvadedIn)
				{
					if (h.Ms < from || h.Ms > to) { continue; }
					dl->AddRect(ImVec2(x(h.Ms) - 2, mainY + mainH - lh), ImVec2(x(h.Ms) + 2, mainY + mainH), kEnemy);
				}
			}
			// health: a step line, 100% at the top
			auto hpY = [&](int32_t hp) { return mainY + lh + 3 + (1.0f - hp / 10000.0f) * (mainH - lh - 6); };
			auto hpAtMs = [&](int32_t ms)
			{
				int32_t hp = 10000;
				for (auto& [m2, v] : p.Hp) { if (m2 > ms) { break; } hp = v; }
				return hp;
			};
			{
				// ArcDPS writes health only now and then, so the last value before a down is often still high: the line drops
				// to 0 at the down. While downed the game shows the downed bar (its own health pool, 75% when the user asked):
				// drawn thin and grey so it doesn't read as health left (the user, 2026-09-30).
				int32_t last = -1;
				for (auto& [ms, hp] : p.Hp) { if (ms <= from) { last = hp; } }
				ImVec2 prev(x(from), last >= 0 ? hpY(last) : -1);
				auto downed = [&](int32_t ms) { return ms >= t && ms < d.S.To; };
				bool dropped = false;
				auto drop = [&]
				{
					if (dropped || prev.y < 0) { return; }
					dl->AddLine(prev, ImVec2(x(t), prev.y), ink, 2);
					dl->AddLine(ImVec2(x(t), prev.y), ImVec2(x(t), hpY(0)), ink, 2);
					prev = ImVec2(x(t), hpY(0));
					dropped = true;
				};
				for (auto& [ms, hp] : p.Hp)
				{
					if (ms <= from || ms > to) { continue; }
					if (ms >= t) { drop(); }
					ImVec2 pt(x(ms), hpY(hp));
					ImU32 col = downed(ms) ? muted : ink;
					float thick = downed(ms) ? 1.0f : 2.0f;
					if (prev.y >= 0) { dl->AddLine(prev, ImVec2(pt.x, prev.y), col, thick); dl->AddLine(ImVec2(pt.x, prev.y), pt, col, thick); }
					prev = pt;
				}
				if (to >= t) { drop(); }
				if (prev.y >= 0) { dl->AddLine(prev, ImVec2(x(to), prev.y), downed(to - 1) ? muted : ink, downed(to - 1) ? 1.0f : 2.0f); }
				SmallText(dl, ImVec2(o.x, hpY(10000) - lh * 0.5f), muted, "100% health");
				SmallText(dl, ImVec2(o.x, mainY + mainH - lh), muted, "damage /1/4 s");
			}
			// the lanes: side by side from each moment's tick, "+n" for what doesn't fit before the next moment
			for (int r = 0; r < kLanes; r++)
			{
				std::vector<const Event*> list;
				for (const Event& e : det.Events) { if (LaneOf(e) == r) { list.push_back(&e); } }
				// within a moment, stability first (it matters most among boons lost)
				float ry = g.y + rail * r + 3, step = lh + 3;
				float drawnTo = g.x - 1000;
				for (size_t a = 0; a < list.size();)
				{
					size_t b = a;
					while (b < list.size() && x(list[b]->Ms) - x(list[a]->Ms) < step) { b++; }
					std::stable_sort(list.begin() + static_cast<long>(a), list.begin() + static_cast<long>(b),
						[](const Event* p1, const Event* p2) { return (p1->Boon == Analysis::kStability) > (p2->Boon == Analysis::kStability); });
					float tick = x(list[a]->Ms);
					float gx = std::max(tick - lh * 0.5f, drawnTo + 2);
					float next = b < list.size() ? x(list[b]->Ms) - lh * 0.5f : g.x + width;
					size_t count = b - a;
					float pillW = ImGui::CalcTextSize("+99").x * 0.85f + 6;
					size_t fit = count;
					if (gx + count * step > next && count > 1)
					{
						fit = static_cast<size_t>(std::max(1.0f, std::floor((next - gx - pillW - 2) / step)));
						fit = std::min(fit, count);
					}
					dl->AddLine(ImVec2(tick, ry + lh + 1), ImVec2(tick, ry + lh + 4), muted);
					for (size_t i = 0; i < fit; i++) { EventIcon(dl, ImVec2(gx + i * step, ry), lh, f, *list[a + i]); }
					drawnTo = gx + fit * step - 3;
					if (fit < count)
					{
						std::string n = "+" + std::to_string(count - fit);
						dl->AddRectFilled(ImVec2(drawnTo + 2, ry + 1), ImVec2(drawnTo + 2 + pillW, ry + lh - 1), IM_COL32(0x3b, 0x42, 0x50, 255));
						SmallText(dl, ImVec2(drawnTo + 5, ry + 1), ink, n);
						drawnTo += pillW + 2;
					}
					a = b;
				}
			}
			// dodges: small, along the graph's top edge, out of the way of the health line
			{
				float dsz = lh * 0.75f, lastX = -1e9f;
				for (const Event& e : det.Events)
				{
					if (LaneOf(e) != -3) { continue; }
					float cx = std::max(x(e.Ms) - dsz * 0.5f, lastX + dsz + 1);
					EventIcon(dl, ImVec2(cx, mainY + 1), dsz, f, e);
					lastX = cx;
				}
			}
			// on the health line: heals, revives, the down, up again, Illusion of Life ending; stacked up when close
			{
				float isz = lh * 0.95f;
				std::vector<std::pair<float, float>> used; // (x, y) of placed icons
				for (const Event& e : det.Events)
				{
					if (LaneOf(e) != -1) { continue; }
					float cx = x(e.Ms) - isz * 0.5f;
					float cy = hpY(hpAtMs(e.Ms)) - isz - 3;
					for (bool clash = true; clash;)
					{
						clash = false;
						for (auto& [ux, uy] : used) { if (std::abs(ux - cx) < isz && std::abs(uy - cy) < isz) { cy = uy - isz - 2; clash = true; } }
					}
					cy = std::max(cy, mainY + 1);
					used.push_back({cx, cy});
					EventIcon(dl, ImVec2(cx, cy), isz, f, e);
				}
			}
			// the down, and the time axis
			// the down: a bright line on a dark edge, so it stays visible over bars and shading (the user, 2026-09-26: it
			// got lost)
			dl->AddLine(ImVec2(x(t), g.y), ImVec2(x(t), mainY + mainH), IM_COL32(0x10, 0x11, 0x14, 255), 5.0f);
			dl->AddLine(ImVec2(x(t), g.y), ImVec2(x(t), mainY + mainH), ink, 2.0f);
			float lastLabel = -1e9f;
			for (int sec = -6; sec <= (to - t) / 1000; sec++)
			{
				std::string label = sec == 0 ? "down" : (sec > 0 ? "+" : "") + std::to_string(sec) + " s";
				float lw = ImGui::CalcTextSize(label.c_str()).x * 0.85f;
				float lx = std::min(x(t + sec * 1000.0) - 8, g.x + width - lw);
				if (lx < lastLabel + 4) { continue; } // no overlapping labels at the right end
				SmallText(dl, ImVec2(lx, mainY + mainH + 2), muted, label);
				lastLabel = lx + lw;
			}
			if (hovered)
			{
				float mxp = ImGui::GetIO().MousePos.x;
				int32_t at = from + static_cast<int32_t>((mxp - g.x) / width * (to - from));
				dl->AddLine(ImVec2(mxp, g.y), ImVec2(mxp, mainY + mainH), muted);
				ImGui::BeginTooltip();
				int32_t hpAt = -1;
				for (auto& [ms, hp] : p.Hp) { if (ms <= at) { hpAt = hp; } }
				ImGui::Text("%s%s", Rel(at - t).c_str(), hpAt >= 0 ? (", health " + std::to_string(hpAt / 100) + "%").c_str() : "");
				std::vector<const Player::TakenHit*> hits;
				for (const auto& h : p.HitsIn) { if (std::abs(h.Ms - at) <= 250) { hits.push_back(&h); } }
				std::sort(hits.begin(), hits.end(), [](const Player::TakenHit* a, const Player::TakenHit* b) { return a->Damage > b->Damage; });
				for (const Player::TakenHit* h : hits)
				{
					SkillIcon(h->Skill, SkillName(f, h->Skill));
					std::string shield = h->Barrier > 0 ? " (" + Num(h->Barrier) + " into barrier)" : std::string();
					ImGui::Text("%s%s %s%s", Num(h->Damage).c_str(), shield.c_str(), SkillName(f, h->Skill).c_str(), h->Enemy >= 0 ? (" (" + f.Enemies[h->Enemy].Spec + ")").c_str() : "");
				}
				// each event with its icon, as on the clock (the user, 2026-10-01: the boons lost and heals were text only)
				for (const Event& e : det.Events)
				{
					if (std::abs(e.Ms - at) > 250 || e.What == Event::E_Burst) { continue; }
					EventIcon(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), lh, f, e);
					ImGui::Dummy(ImVec2(lh, lh));
					ImGui::SameLine(0, 4);
					ImGui::TextUnformatted(e.Text.c_str());
				}
				ImGui::EndTooltip();
			}
			ImGui::SetCursorScreenPos(ImVec2(o.x, mainY + mainH + lh + 4));
			// The legend: shapes and icons, never colour alone
			ImVec2 lp = ImGui::GetCursorScreenPos();
			float lx = lp.x;
			float right = lp.x + labelW + width;
			auto item = [&](auto aDraw, const char* aText)
			{
				if (lx + lh + 4 + ImGui::CalcTextSize(aText).x > right) { lx = lp.x; lp.y += lh + 4; }
				aDraw(ImVec2(lx, lp.y));
				lx += lh + 4;
				dl->AddText(ImVec2(lx, lp.y), muted, aText);
				lx += ImGui::CalcTextSize(aText).x + 14;
			};
			item([&](ImVec2 q) { BoonIconAt(dl, q, lh, Analysis::kStability, true); }, "boon stripped or corrupted");
			item([&](ImVec2 q) { IconAt(dl, q, lh, -200 - Analysis::kStability, "Stability", kYou); }, "stability given (grey frame: missed them)");
			item([&](ImVec2 q) { IconAt(dl, q, lh, -300, "Healing", kYou); }, "healed");
			item([&](ImVec2 q) { IconAt(dl, q, lh, 23275, "Dodge", kYou); }, "dodged (top edge)");
			item([&](ImVec2 q) { IconAt(dl, q, lh, -302, "Resurrect", kYou); }, "revived, up again (grey frame: a revive skill that didn't get them up)");
			item([&](ImVec2 q) { dl->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q.x + lh - 2, q.y + lh), kEnemy); }, "damage to health");
			item([&](ImVec2 q) { dl->AddRectFilled(ImVec2(q.x + 2, q.y + 2), ImVec2(q.x + lh - 2, q.y + lh), IM_COL32(217, 89, 38, 110)); }, "into barrier");
			item([&](ImVec2 q) { Dashed(dl, q.x + 2, q.y + 1, q.y + lh, kEnemy); Dashed(dl, q.x + lh - 2, q.y + 1, q.y + lh, kEnemy); }, "the burst");
			item([&](ImVec2 q) { dl->AddRect(ImVec2(q.x + 5, q.y + 2), ImVec2(q.x + 9, q.y + lh), kEnemy); }, "hit evaded");
			item([&](ImVec2 q) { dl->AddRectFilled(ImVec2(q.x, q.y + 2), ImVec2(q.x + lh, q.y + lh - 2), IM_COL32(0x3b, 0x3f, 0x46, 200)); }, "downed");
			item([&](ImVec2 q) { dl->AddRectFilled(ImVec2(q.x, q.y + 2), ImVec2(q.x + lh, q.y + lh - 2), IM_COL32(57, 135, 229, 90)); }, "stability on");
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, lp.y));
			ImGui::Dummy(ImVec2(1, lh));
		}

		// The list at the left: every down, grouped by the enemy spike it fell in
		void List(const Ctx& c, const std::vector<Down>& aDowns, float aWidth, const std::string& aShown)
		{
			const Fight& f = *c.F;
			State& s = S();
			ImGui::BeginChild("downlist", ImVec2(aWidth, 0), false);
			// filter
			auto seg = [](const char* aLabel, bool aOn)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(aOn ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
				bool clicked = ImGui::SmallButton(aLabel);
				ImGui::PopStyleColor();
				return clicked;
			};
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
			if (seg("Everyone", s.DeathFilter == 0)) { s.DeathFilter = 0; }
			if (c.MeRaw)
			{
				ImGui::SameLine();
				if (seg("You", s.DeathFilter == 1)) { s.DeathFilter = 1; s.DeathKey.clear(); }
				ImGui::SameLine();
				std::string sg = "Sg " + std::to_string(c.MeRaw->Subgroup);
				if (seg(sg.c_str(), s.DeathFilter == 2)) { s.DeathFilter = 2; s.DeathKey.clear(); }
			}
			// the revive skills and the revive order get the right side (the user, 2026-09-26: at the bottom they were hidden)
			ImGui::SameLine(0, 8);
			if (seg("Revives", s.DeathFilter == 3)) { s.DeathFilter = 3; }
			ImGui::PopStyleVar();
			// groups in time order: each enemy spike's downs, and the downs between spikes (the user, 2026-09-25: a down
			// outside a spike belongs where it happened, not at the bottom). Key -1: outside a spike.
			std::vector<std::pair<int64_t, std::vector<const Down*>>> groups;
			for (const Down& d : aDowns)
			{
				int64_t spike = SpikeOf(f.TheirSpikesMs, d.S.From);
				if (groups.empty() || groups.back().first != spike) { groups.push_back({spike, {}}); }
				groups.back().second.push_back(&d);
			}
			float lh = ImGui::GetTextLineHeight();
			auto items = [&](const std::vector<const Down*>& aList)
			{
				for (const Down* d : aList)
				{
					std::string key = DownKey(f, *d);
					ImGui::PushID(key.c_str());
					ImVec2 p = ImGui::GetCursorScreenPos();
					if (ImGui::Selectable("##d", key == aShown, 0, ImVec2(0, lh))) { s.DeathKey = key; if (s.DeathFilter == 3) { s.DeathFilter = 0; } }
					ImDrawList* dl = ImGui::GetWindowDrawList();
					dl->AddText(p, ImGui::GetColorU32(kMuted), Duration(d->S.From).c_str());
					// got up: a blue circle; died: an orange cross (the word is in the detail)
					ImVec2 m(p.x + 48, p.y + lh * 0.5f);
					if (d->Died) { dl->AddLine(ImVec2(m.x - 4, m.y - 4), ImVec2(m.x + 4, m.y + 4), kEnemy, 2); dl->AddLine(ImVec2(m.x - 4, m.y + 4), ImVec2(m.x + 4, m.y - 4), kEnemy, 2); }
					else { dl->AddCircleFilled(m, 4, kYou); }
					dl->AddText(ImVec2(p.x + 60, p.y), ImGui::GetColorU32(ImGuiCol_Text), (d->P->Name + (d->P->Pov ? " (you)" : "")).c_str());
					if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s, subgroup %d: %s", d->P->Name.c_str(), d->P->Subgroup, d->Died ? "died" : "got up"); }
					ImGui::PopID();
				}
			};
			for (auto& [t, list] : groups)
			{
				int died = static_cast<int>(std::count_if(list.begin(), list.end(), [](const Down* d) { return d->Died; }));
				ImGui::Spacing();
				if (t >= 0) { ImGui::TextColored(kMuted, "Enemy spike %s: %d downed, %d died", Duration(t).c_str(), static_cast<int>(list.size()), died); }
				else { ImGui::TextColored(kMuted, "Outside a spike: %d downed, %d died", static_cast<int>(list.size()), died); }
				items(list);
			}
			ImGui::Spacing();
			ImGui::TextColored(kMuted, "Circle: got up. Cross: died.");
			ImGui::EndChild();
		}
	}

	void DeathsTab(const Ctx& c)
	{
		const Fight& f = *c.F;
		State& s = S();
		std::vector<Down> all = Downs(f);
		std::vector<Down> downs;
		for (const Down& d : all)
		{
			if (s.DeathFilter == 1 && (!c.MeRaw || d.P != c.MeRaw)) { continue; }
			if (s.DeathFilter == 2 && (!c.MeRaw || d.P->Subgroup != c.MeRaw->Subgroup)) { continue; }
			downs.push_back(d);
		}
		if (all.empty())
		{
			Answer("None of ours went down this round.");
			ImGui::Spacing();
			RevivesTable(c);
			return;
		}
		// The down shown: the one picked, else the first of the enemy spike opened from Round, else yours, else the first
		const Down* shown = nullptr;
		for (const Down& d : downs) { if (DownKey(f, d) == s.DeathKey) { shown = &d; } }
		if (!shown && s.DeathSpike >= 0) { for (const Down& d : downs) { if (!shown && SpikeOf(f.TheirSpikesMs, d.S.From) == s.DeathSpike) { shown = &d; } } }
		if (!shown && c.MeRaw) { for (const Down& d : downs) { if (!shown && d.P == c.MeRaw) { shown = &d; } } }
		if (!shown && !downs.empty()) { shown = &downs[0]; }
		if (shown) { s.DeathKey = DownKey(f, *shown); }
		s.DeathSpike = -1;

		float listW = 250;
		List(c, downs, listW, s.DeathFilter == 3 ? std::string() : shown ? DownKey(f, *shown) : std::string());
		ImGui::SameLine(0, 10);
		ImGui::BeginChild("downdetail", ImVec2(0, 0), false);
		if (s.DeathFilter == 3) { RevivesTable(c); ImGui::EndChild(); return; }
		if (!shown) { ImGui::TextColored(kMuted, "No downs for this filter."); ImGui::Spacing(); RevivesTable(c); ImGui::EndChild(); return; }
		static std::string cachedKey;
		static Detail cached;
		std::string key = DownKey(f, *shown) + "|" + std::to_string(DataVersion());
		if (key != cachedKey) { cached = Explain(f, *shown); cachedKey = key; }
		const Detail& det = cached;
		ImGui::SetWindowFontScale(1.1f);
		Answer(det.Verdict);
		ImGui::SetWindowFontScale(1.0f);
		ImGui::Spacing();
		// first how they went down and what hit them, then the clock and every event (the user's v7 pick, proposal A)
		ImGui::TextColored(kMuted, "How they went down");
		Steps(f, det);
		ImGui::Spacing();
		TopSkills(f, det);
		ImGui::Spacing();
		ImGui::TextColored(kMuted, "The 6 s on a clock, then second by second");
		DownClock(f, *shown, det);
		ImGui::TextColored(kMuted, "Dodges:");
		ImGui::SameLine();
		ImGui::TextUnformatted(det.DodgeLine.c_str());
		ImGui::Separator();
		// The same events as sentences, in order
		float lh = ImGui::GetTextLineHeight();
		for (const Event& e : det.Events)
		{
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddText(p, ImGui::GetColorU32(kMuted), Rel(e.Ms - shown->S.From).c_str());
			EventIcon(dl, ImVec2(p.x + 62, p.y), lh, f, e);
			ImGui::SetCursorScreenPos(ImVec2(p.x + 62 + lh + 8, p.y));
			ImGui::PushTextWrapPos(0.0f);
			if (e.What == Event::E_Burst || e.What == Event::E_Down) { ImGui::TextColored(e.What == Event::E_Down && shown->Died ? kWorseV : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", e.Text.c_str()); }
			else { ImGui::TextUnformatted(e.Text.c_str()); }
			ImGui::PopTextWrapPos();
		}
		ImGui::Spacing();
		ImGui::Separator();
		if (ImGui::SmallButton("Revive skills and the revive order >")) { s.DeathFilter = 3; }
		ImGui::EndChild();
	}
}
