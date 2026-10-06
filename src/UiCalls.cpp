// This round's calls (the Summary): how the squad's key skills were used, each judged by its own rule. Which skills,
// and the rules, come from a month of logs and the user (notes/KEY_SKILLS.md, 2026-10-03): wells together; Warrior,
// Chronomancer and Elementalist burst with the wells; Winds of Disenchantment one or two per spike; Spinal Shivers
// together at the opening on one target; Battle Standard on a down; Tale of the August Queen, stability and heals
// before their spikes; revives after downs; Crescendo and Continuum Split with a spike, either side's.
#include "UiCommon.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <cstdio>
#include <functional>

namespace Ui
{
	namespace
	{
		struct Use { int32_t Ms; const Player* By; int32_t Skill; };

		// casts of any of these skills (by name: every id of it), by anyone of ours, in time order
		std::vector<Use> CastsNamed(const Fight& f, const std::vector<const char*>& aNames, uint32_t aProf = 0)
		{
			std::vector<Use> out;
			for (const Player& p : f.Players)
			{
				if (aProf && p.ProfId != aProf) { continue; }
				for (auto& [sk, row] : p.Skills)
				{
					auto n = f.SkillNames.find(sk);
					if (sk <= 0 || n == f.SkillNames.end() || std::none_of(aNames.begin(), aNames.end(), [&](const char* x) { return n->second == x; })) { continue; }
					for (int32_t ms : row.CastMs) { out.push_back({ms, &p, sk}); }
				}
			}
			std::sort(out.begin(), out.end(), [](const Use& a, const Use& b) { return a.Ms < b.Ms; });
			return out;
		}

		// a burst of instant skills, which the log has no cast for (a Chronomancer's shatters: the user, 2026-10-06, the
		// card said 0 of 1 while their damage lined up with the wells), by its hits on players: each run of the skills'
		// hits (gaps under 2 s) at its first hit, in time order
		std::vector<Use> HitsNamed(const Fight& f, const std::vector<const char*>& aNames, uint32_t aProf)
		{
			std::vector<Use> out;
			for (const Player& p : f.Players)
			{
				if (aProf && p.ProfId != aProf) { continue; }
				int32_t last = INT32_MIN / 2;
				for (const auto& h : p.HitsOut)
				{
					auto n = f.SkillNames.find(h.Skill);
					if (n == f.SkillNames.end() || std::none_of(aNames.begin(), aNames.end(), [&](const char* x) { return n->second == x; })) { continue; }
					if (h.Ms - last > 2000) { out.push_back({h.Ms, &p, h.Skill}); }
					last = h.Ms;
				}
			}
			std::sort(out.begin(), out.end(), [](const Use& a, const Use& b) { return a.Ms < b.Ms; });
			return out;
		}

		std::string Name(const Fight& f, int32_t aSkill)
		{
			auto it = f.SkillNames.find(aSkill);
			return it == f.SkillNames.end() ? std::to_string(aSkill) : it->second;
		}

		std::string Offset(int32_t aMs)
		{
			char b[16];
			std::snprintf(b, sizeof b, "%+.1f s", aMs / 1000.0);
			std::string s = b;
			return s;
		}

		// the nearest of the reference moments, and how far from it
		std::pair<int64_t, int32_t> Nearest(const std::vector<int64_t>& aRefs, int32_t aMs)
		{
			int64_t best = -1;
			for (int64_t t : aRefs) { if (best < 0 || std::llabs(aMs - t) < std::llabs(aMs - best)) { best = t; } }
			return {best, best < 0 ? 0 : static_cast<int32_t>(aMs - best)};
		}

		void Rate(CallCard& c, double aShare, const char* aGood, const char* aMid, const char* aBad)
		{
			c.Verdict = aShare >= 0.75 ? aGood : aShare >= 0.5 ? aMid : aBad;
			c.Kind = aShare >= 0.75 ? 0 : aShare >= 0.5 ? 1 : 2;
		}

		std::string Names(const std::vector<std::string>& aNames, size_t aMax = 3)
		{
			std::vector<std::string> u;
			for (const std::string& n : aNames) { if (std::find(u.begin(), u.end(), n) == u.end()) { u.push_back(n); } }
			std::string out;
			for (size_t i = 0; i < u.size() && i < aMax; i++) { out += (i ? ", " : "") + u[i]; }
			if (u.size() > aMax) { out += " and " + std::to_string(u.size() - aMax) + " more"; }
			return out;
		}

		// An icon for a skill by name: the player's own id for it, else any id of that name in this log, else a made-up
		// id that the icon lookup finds by name (a follow-up nobody cast this round still shows its icon)
		int32_t IconId(const Fight& f, const Player* aBy, const std::string& aName)
		{
			if (aBy) { for (auto& [sk, row] : aBy->Skills) { if (sk > 0 && Name(f, sk) == aName) { return sk; } } }
			for (auto& [sk, n] : f.SkillNames) { if (sk > 0 && n == aName) { return sk; } }
			return -100000 - static_cast<int32_t>(std::hash<std::string>{}(aName) % 100000);
		}

		// downed or dead at that moment
		bool DownAt(const Player& p, int32_t aMs)
		{
			for (const auto& sp : p.DownSpans) { if (sp.From <= aMs && sp.To >= aMs) { return true; } }
			return false;
		}

		const Analysis::Player::Point* EnemyPos(const Fight::Enemy& e, int32_t aMs)
		{
			const Analysis::Player::Point* best = nullptr;
			for (const auto& p : e.Pos) { if (!best || std::abs(p.Ms - aMs) < std::abs(best->Ms - aMs)) { best = &p; } }
			return best && std::abs(best->Ms - aMs) <= 2000 ? best : nullptr;
		}

		constexpr int32_t kBefore = 1000, kAfter = 2500; // a burst on the call: this far before or after the wells

		std::vector<CallCard> Build(const Fight& f)
		{
			std::vector<CallCard> out;
			// the wells' moments: casts within 3 s of each other are one call, at their median
			std::vector<Use> wells = CastsNamed(f, {"Well of Corruption", "Well of Suffering"});
			// a cast that never went off isn't a well: one of the player's own pulses follows within 2.5 s, unless their well
			// hit no player all round (5 Oct 22:00: a Well of Suffering stopped at 449 of 700 ms by another command counted as
			// cast with the call, and the real one 10 s later as a call of its own)
			wells.erase(std::remove_if(wells.begin(), wells.end(), [&](const Use& u)
			{
				const std::string name = Name(f, u.Skill);
				bool any = false;
				for (const auto& h : u.By->HitsOut)
				{
					if (Name(f, h.Skill) != name) { continue; }
					any = true;
					if (h.Ms >= u.Ms && h.Ms <= u.Ms + 2500) { return false; }
				}
				return any;
			}), wells.end());
			// a call: wells within 3 s of its first (chained gaps merged waves into 12 s) from two Necromancers or more (one,
			// when only one cast wells). A lone late or early well belongs to the call it was meant for, 5 s before its first
			// cast to 15 s after, as off time; it isn't a call of its own (the user, 2026-10-06: three "calls" in 10 s were one
			// call and two stragglers, and the Chronomancers' bursts were timed against the stragglers)
			struct WellCall { std::vector<Use> Casts; int64_t At = 0, From = 0, To = 0; bool Lone = false; };
			std::vector<WellCall> wellCalls;
			{
				std::vector<std::vector<Use>> clusters;
				std::set<const Player*> necroSet;
				for (const Use& u : wells)
				{
					necroSet.insert(u.By);
					if (clusters.empty() || u.Ms - clusters.back().front().Ms > 3000) { clusters.push_back({}); }
					clusters.back().push_back(u);
				}
				const size_t need = std::min<size_t>(2, necroSet.size());
				std::vector<Use> strays;
				for (auto& cl : clusters)
				{
					std::set<const Player*> by;
					for (const Use& u : cl) { by.insert(u.By); }
					if (by.size() < need) { strays.insert(strays.end(), cl.begin(), cl.end()); continue; }
					wellCalls.push_back({cl, cl[cl.size() / 2].Ms, cl.front().Ms, cl.back().Ms});
				}
				for (const Use& u : strays)
				{
					WellCall* home = nullptr;
					for (WellCall& wc : wellCalls)
					{
						if (u.Ms >= wc.From - 5000 && u.Ms <= wc.From + 15000 && (!home || std::abs(u.Ms - wc.At) < std::abs(u.Ms - home->At))) { home = &wc; }
					}
					// with no call near: a well cast alone, shown as off, and no burst is timed against it
					if (home) { home->Casts.push_back(u); }
					else { wellCalls.push_back({{u}, u.Ms, u.Ms, u.Ms, true}); }
				}
				std::sort(wellCalls.begin(), wellCalls.end(), [](const WellCall& a, const WellCall& b) { return a.At < b.At; });
				for (WellCall& wc : wellCalls) { std::sort(wc.Casts.begin(), wc.Casts.end(), [](const Use& a, const Use& b) { return a.Ms < b.Ms; }); }
			}
			std::vector<std::vector<Use>> calls;
			std::vector<int64_t> wellAt, callAt; // every row of the wells card; the calls bursts are timed against
			for (const WellCall& wc : wellCalls)
			{
				calls.push_back(wc.Casts);
				wellAt.push_back(wc.At);
				if (!wc.Lone) { callAt.push_back(wc.At); }
			}
			const bool byWells = !callAt.empty();
			const std::vector<int64_t>& ourRefs = byWells ? callAt : f.OurSpikesMs;
			const char* refWord = byWells ? "the wells" : "ally spikes";

			// ---- in our spikes ------------------------------------------------------------------------------------
			if (!wells.empty())
			{
				CallCard c;
				c.Key = "wells"; c.Group = 0; c.Name = "Wells"; c.Should = "together"; c.Skill = wells[0].Skill;
				int together = 0, downs = 0, strips = 0, leftOut = 0;
				// a grid: a column per Necromancer who cast a well this round (by subgroup, then name), and per call their
				// two wells side by side: cast on time, cast off time, left out, or down at the time (the user, 2026-10-05:
				// both wells belong together, and who left one out couldn't be seen)
				std::vector<const Player*> necros;
				for (const Use& u : wells) { if (std::find(necros.begin(), necros.end(), u.By) == necros.end()) { necros.push_back(u.By); } }
				std::sort(necros.begin(), necros.end(), [](const Player* a, const Player* b) { return a->Subgroup != b->Subgroup ? a->Subgroup < b->Subgroup : a->Name < b->Name; });
				for (const Player* p : necros) { c.Columns.push_back(p->Name); }
				const char* const kWells[] = {"Well of Corruption", "Well of Suffering"};
				for (size_t k = 0; k < calls.size(); k++)
				{
					// a row per call, every well of it on one strip around the call's middle cast; together: within 1 s of it (the
					// user, 2026-10-05: as text alone it couldn't be read at a glance)
					CallRow r;
					int in = 0, callDowns = 0;
					for (const Use& u : calls[k])
					{
						int32_t o = static_cast<int32_t>(u.Ms - wellAt[k]);
						bool ok = std::abs(o) <= 1000 && !wellCalls[k].Lone;
						in += ok;
						r.Marks.push_back({o, ok, u.By->Name, Name(f, u.Skill)});
					}
					together += in;
					for (int32_t d : f.EnemyDownMs) { callDowns += d >= wellCalls[k].From && d <= wellCalls[k].To + 5000; }
					downs += callDowns;
					r.Who = std::to_string(calls[k].size()) + (calls[k].size() == 1 ? " well" : " wells");
					r.Ms = static_cast<int32_t>(wellAt[k]);
					r.Ref = Duration(wellAt[k]);
					r.What = wellCalls[k].Lone ? std::string("alone") : std::to_string(in) + " of " + std::to_string(calls[k].size());
					r.Good = in == static_cast<int>(calls[k].size());
					r.Downs = callDowns;
					for (const Player* p : necros)
					{
						std::vector<ChainStep> cell;
						for (const char* well : kWells)
						{
							ChainStep st;
							st.Name = well;
							st.Skill = IconId(f, p, well);
							const Use* cast = nullptr;
							for (const Use& u : calls[k]) { if (u.By == p && Name(f, u.Skill) == well) { cast = &u; break; } }
							// a well cast alone: only its own icon; nobody else left anything out of it
							if (wellCalls[k].Lone && !cast) { continue; }
							if (wellCalls[k].Lone)
							{
								st.Skill = cast->Skill;
								st.State = CS_OffTime;
								st.Tip = p->Name + ": cast alone, no call of two Necromancers or more within 5 s before to 15 s after";
							}
							else if (cast)
							{
								int32_t o = static_cast<int32_t>(cast->Ms - wellAt[k]);
								st.Skill = cast->Skill;
								st.State = std::abs(o) <= 1000 ? CS_Done : CS_OffTime;
								st.Tip = p->Name + ": " + Offset(o) + " from the call's middle cast";
							}
							else if (DownAt(*p, static_cast<int32_t>(wellAt[k]))) { st.State = CS_Skipped; st.Tip = p->Name + " was down then"; }
							else { st.State = CS_Missed; st.Tip = p->Name + " didn't cast it in this call"; leftOut++; }
							cell.push_back(st);
						}
						r.Cells.push_back(cell);
					}
					r.WinFrom = -1000;
					r.WinTo = 1000;
					c.Rows.push_back(r);
				}
				for (const Player& p : f.Players) { for (auto& [sk, row] : p.Skills) { auto n = f.SkillNames.find(sk); if (n != f.SkillNames.end() && (n->second == "Well of Corruption" || n->second == "Well of Suffering")) { strips += row.Strips; } } }
				c.Num = together; c.Den = static_cast<int>(wells.size());
				c.Line = std::to_string(together) + " of " + std::to_string(wells.size()) + " together (within 1 s of the call)";
				c.Short = std::to_string(together) + " of " + std::to_string(wells.size()) + " together";
				if (strips) { c.Line += " \xc2\xb7 " + std::to_string(strips) + " boons stripped"; }
				c.Line += " \xc2\xb7 " + std::to_string(downs) + (downs == 1 ? " enemy down" : " enemy downs") + " within 5 s";
				if (leftOut) { c.Line += " \xc2\xb7 " + std::to_string(leftOut) + (leftOut == 1 ? " well left out" : " wells left out"); }
				Rate(c, double(together) / wells.size(), "together", "partly", "split");
				c.Detail = "A call: the wells of two Necromancers or more within 3 s of its first; a lone well up to 15 s later counts with it, off time, and one with no "
					"call near is shown alone. A cancelled cast (no pulse after it) doesn't count. The strip: each well against the call's middle cast; together: within 1 s of it, the shaded part. "
					"Then each Necromancer's two wells: lit, cast together; a gold frame, cast but off time; struck through in red, left out; in grey, they were down. "
					"The triangle: enemies downed in the 5 s after the call.";
				out.push_back(c);
			}
			// burst into the wells (or into our spikes, with no wells)
			// aByHits: timed by the first hit (instant skills); hits land after the cast, so on the call runs to 3 s after
			// (October: Chronomancers' first Split Second or Time Bomb hits spread from 1 s before the wells to 3 s after)
			auto into = [&](const char* aKey, const char* aName, std::vector<const char*> aSkills, uint32_t aProf, bool aByHits = false)
			{
				std::vector<Use> casts = aByHits ? HitsNamed(f, aSkills, aProf) : CastsNamed(f, aSkills, aProf);
				const int32_t after = aByHits ? 3000 : kAfter;
				if (casts.empty()) { return; }
				CallCard c;
				c.Key = aKey; c.Group = 0; c.Name = aName; c.Should = std::string("with ") + refWord; c.Skill = casts[0].Skill;
				int near = 0, on = 0, late = 0;
				std::vector<std::string> lateNames;
				// per player and call, the cast nearest the call's on-time window (the middle of -1 to +2.5 s)
				std::map<std::pair<const Player*, int64_t>, std::pair<const Use*, int32_t>> best;
				for (const Use& u : casts)
				{
					auto [ref, off] = Nearest(ourRefs, u.Ms);
					if (ref < 0 || std::abs(off) > 5000) { continue; }
					auto key = std::make_pair(u.By, ref);
					auto it = best.find(key);
					const int32_t mid = (after - kBefore) / 2;
					if (it == best.end() || std::abs(off - mid) < std::abs(it->second.second - mid)) { best[key] = {&u, off}; }
				}
				for (auto& [key, v] : best)
				{
					const Use& u = *v.first;
					int32_t off = v.second;
					near++;
					bool ok = off >= -kBefore && off <= after;
					on += ok;
					if (!ok && off > 0) { late++; lateNames.push_back(u.By->Name); }
					CallRow r{u.By->Name, Name(f, u.Skill), u.Ms, Duration(key.second), Offset(off), ok, off};
					r.Section = std::string(byWells ? "Wells at " : "Spike at ") + Duration(key.second);
					r.WinTo = after;
					r.By = u.By;
					r.At = static_cast<int32_t>(key.second);
					c.Rows.push_back(r);
				}
				std::sort(c.Rows.begin(), c.Rows.end(), [](const CallRow& a, const CallRow& b) { return a.Ms - a.Offset != b.Ms - b.Offset ? a.Ms - a.Offset < b.Ms - b.Offset : a.Ms < b.Ms; });
				c.Num = on; c.Den = near;
				const std::string window = aByHits ? " (first hit 1 s before to 3 s after)" : " (1 s before to 2.5 s after)";
				if (!near) { c.Line = std::to_string(casts.size()) + (aByHits ? " bursts" : " cast") + ", none within 5 s of " + refWord; c.Short = "none near " + std::string(refWord); c.Verdict = "off the call"; c.Kind = 2; }
				else
				{
					c.Line = std::to_string(on) + " of " + std::to_string(near) + " players' calls with " + refWord + window;
					c.Short = std::to_string(on) + " of " + std::to_string(near) + " on the call";
					if (!lateNames.empty()) { c.Line += " \xc2\xb7 late: " + Names(lateNames); }
					Rate(c, double(on) / near, "on the call", "partly", late * 2 >= near - on ? "late" : "early");
				}
				c.Detail = aByHits ? std::string("For each call (") + refWord + "), each player's best-timed burst within 5 s of it, by its first hit: the log has no cast for "
					"instant skills like shatters. On the call: the first hit from 1 s before to 3 s after."
					: std::string("For each call (") + refWord + "), each player's best-timed cast within 5 s of it; on the call: 1 s before to 2.5 s after (the wells pulse after their cast)";
				out.push_back(c);
			};
			// Warriors, one card in two parts (the user, 2026-10-04), as a squad Warrior explained it: the spear opener (spear 3,
			// Disrupting Throw, only with Signet of Might and synced with the other Warriors; then spear 2, 4 and F1) and the
			// melee burst (the Hydromancy swap, then F1 Bloodthirster first) into the wells. Harrier's Toss is poke, not judged.
			{
				std::vector<Use> dt = CastsNamed(f, {"Disrupting Throw"}, 2);
				std::vector<Use> burst;
				for (const Use& u : CastsNamed(f, {"Bloodthirster"}, 2))
				{
					auto sw = u.By->Skills.find(-2); // weapon swaps
					if (sw == u.By->Skills.end()) { continue; }
					int32_t lastSwap = INT32_MIN;
					for (int32_t ms : sw->second.CastMs) { if (ms <= u.Ms) { lastSwap = std::max(lastSwap, ms); } }
					if (lastSwap == INT32_MIN || u.Ms - lastSwap > 3000) { continue; }
					if (std::none_of(burst.begin(), burst.end(), [&](const Use& b) { return b.By == u.By && b.Ms >= lastSwap && b.Ms < u.Ms; })) { burst.push_back(u); }
				}
				if (!dt.empty() || !burst.empty())
				{
					CallCard c;
					c.Key = "warrior"; c.Group = 0; c.Name = "Warrior"; c.Should = "spear opener, then melee burst with " + std::string(refWord);
					c.Skill = !burst.empty() ? burst[0].Skill : dt[0].Skill;
					// the opener (a squad Warrior): spear 3 Disrupting Throw with Signet of Might, synced with the other Warriors, then
					// spear 2 Maiming Spear, 4 Spearmarshal's Support and F1 Harrier's Toss. Openers: Disrupting Throws within 3 s of
					// the first, at their middle cast. "No signet" only when the signet was ready (16 s since its last use: 20 s
					// recharge, 16 s with Signet Mastery) and not used (the user, 2026-10-04).
					int withSig = 0, synced = 0, openerOk = 0, sigReadyMissed = 0;
					std::vector<std::vector<const Use*>> openers;
					for (const Use& u : dt)
					{
						if (openers.empty() || u.Ms - openers.back().front()->Ms > 3000) { openers.push_back({}); }
						openers.back().push_back(&u);
					}
					auto castsOf = [&](const Player* p, const char* aName)
					{
						std::vector<int32_t> out;
						for (auto& [sk, row] : p->Skills) { if (Name(f, sk) == aName) { out.insert(out.end(), row.CastMs.begin(), row.CastMs.end()); } }
						return out;
					};
					for (auto& op : openers)
					{
						const int32_t at = op[op.size() / 2]->Ms;
						for (const Use* u : op)
						{
							// the signet by its effect (the user, 2026-10-05: one used before the round showed as "not used"): its
							// Unblockable on at the throw = used; its ready buff on = ready and not used; neither = recharging. A log
							// without the buffs falls back to the casts (used in the 3 s before; ready 16 s after the last).
							auto on = [&](const std::vector<std::pair<int32_t, int32_t>>& aSpans, int32_t aFrom, int32_t aTo)
							{
								return std::any_of(aSpans.begin(), aSpans.end(), [&](const std::pair<int32_t, int32_t>& sp) { return sp.first <= aTo && sp.second >= aFrom; });
							};
							std::vector<int32_t> sigs = castsOf(u->By, "Signet of Might");
							bool sig = std::any_of(sigs.begin(), sigs.end(), [&](int32_t ms) { return ms >= u->Ms - 3000 && ms <= u->Ms + 200; }) ||
								on(u->By->UnblockableOn, u->Ms - 300, u->Ms + 200);
							bool ready = false, onBar = true;
							if (!u->By->MightSignetReady.empty() || !u->By->UnblockableOn.empty()) { ready = !sig && on(u->By->MightSignetReady, u->Ms, u->Ms); }
							else if (!sigs.empty())
							{
								int32_t last = INT32_MIN;
								for (int32_t ms : sigs) { if (ms < u->Ms - 3000) { last = std::max(last, ms); } }
								ready = last == INT32_MIN || u->Ms - last >= 16000;
							}
							else { onBar = false; }
							bool sync = std::any_of(dt.begin(), dt.end(), [&](const Use& o) { return o.By != u->By && std::abs(o.Ms - u->Ms) <= 1000; });
							// the opener as a chain of icons: Signet of Might, the throw, then the follow-ups in the 4 s after (the user,
							// 2026-10-05: icons with arrows, the names on hover, no slot numbers and no sentence)
							bool sigOk = sig || !ready;
							withSig += sig; synced += sync; openerOk += sigOk && sync;
							sigReadyMissed += !sigOk;
							std::vector<ChainStep> chain;
							{
								ChainStep st;
								st.Name = "Signet of Might";
								st.Skill = IconId(f, u->By, st.Name);
								st.State = sig ? CS_Done : ready ? CS_Missed : CS_Skipped;
								st.Tip = sig ? "used: its Unblockable was on at the throw" : ready ? "it was ready and wasn't used" :
									onBar ? "on cooldown" : "not on their bar this round";
								chain.push_back(st);
							}
							{
								ChainStep st;
								st.Name = "Disrupting Throw";
								st.Skill = u->Skill;
								st.Tip = Offset(u->Ms - at) + " from the opener's middle throw; " + (sync ? "with another Warrior's (within 1 s)" : "alone: no other Warrior's within 1 s");
								chain.push_back(st);
							}
							for (const char* name : {"Maiming Spear", "Spearmarshal's Support", "Harrier's Toss"})
							{
								std::vector<int32_t> cs = castsOf(u->By, name);
								auto it = std::find_if(cs.begin(), cs.end(), [&](int32_t ms) { return ms >= u->Ms && ms <= u->Ms + 4000; });
								ChainStep st;
								st.Name = name;
								st.Skill = IconId(f, u->By, name);
								st.State = it != cs.end() ? CS_Done : CS_Missed;
								st.Tip = it != cs.end() ? Offset(*it - u->Ms) + " after the throw" : "not cast in the 4 s after the throw";
								chain.push_back(st);
							}
							CallRow r{u->By->Name, "Disrupting Throw", u->Ms, Duration(at), Offset(u->Ms - at) + (sync ? std::string() : std::string("  alone")), sigOk && sync, u->Ms - at};
							r.NoteBad = !sync;
							r.Chain = chain;
							r.Part = "SPEAR OPENER";
							r.Section = "Opener at " + Duration(at);
							r.By = u->By;
							r.At = at;
							c.Rows.push_back(r);
						}
					}
					// the melee burst against the wells
					int near = 0, on = 0, late = 0;
					std::vector<std::string> lateNames;
					std::vector<CallRow> melee;
					for (const Use& u : burst)
					{
						auto [ref, off] = Nearest(ourRefs, u.Ms);
						if (ref < 0 || std::abs(off) > 5000) { continue; }
						near++;
						bool ok = off >= -kBefore && off <= kAfter;
						on += ok;
						if (!ok && off > 0) { late++; lateNames.push_back(u.By->Name); }
						CallRow r{u.By->Name, "Bloodthirster", u.Ms, Duration(ref), Offset(off), ok, off};
						r.Part = "MELEE BURST";
						r.Section = std::string(byWells ? "Wells at " : "Spike at ") + Duration(ref);
						r.By = u.By;
						r.At = static_cast<int32_t>(ref);
						melee.push_back(r);
					}
					std::sort(melee.begin(), melee.end(), [](const CallRow& a, const CallRow& b) { return a.Ms - a.Offset != b.Ms - b.Offset ? a.Ms - a.Offset < b.Ms - b.Offset : a.Ms < b.Ms; });
					c.Rows.insert(c.Rows.end(), melee.begin(), melee.end());
					c.Num = openerOk + on; c.Den = static_cast<int>(dt.size()) + near;
					std::string opener = dt.empty() ? std::string("no spear opener") : "opener: " + std::to_string(openerOk) + " of " + std::to_string(dt.size()) + " good (" +
						std::to_string(sigReadyMissed) + " without a ready signet, " + std::to_string(static_cast<int>(dt.size()) - synced) + " alone)";
					std::string meleeLine = near ? "melee burst: " + std::to_string(on) + " of " + std::to_string(near) + " on the call" + (lateNames.empty() ? std::string() : " (late: " + Names(lateNames) + ")")
						: std::string("no melee burst near ") + refWord;
					c.Line = opener + " \xc2\xb7 " + meleeLine;
					c.Short = (dt.empty() ? std::string() : "opener " + std::to_string(openerOk) + "/" + std::to_string(dt.size())) + (dt.empty() || !near ? "" : " \xc2\xb7 ") +
						(near ? "burst " + std::to_string(on) + "/" + std::to_string(near) : std::string());
					// the verdict: the weaker part names it
					double o = dt.empty() ? 1.0 : double(openerOk) / dt.size(), m = near ? double(on) / near : 1.0;
					if (dt.empty() && !near) { c.Verdict = "off the call"; c.Kind = 2; }
					else if (o <= m) { Rate(c, o, "on the call", "opener partly", sigReadyMissed * 2 >= static_cast<int>(dt.size()) - openerOk ? "opener: no signet" : "opener not synced"); }
					else { Rate(c, m, "on the call", "burst partly", late * 2 >= near - on ? "burst late" : "burst early"); }
					// each part explained on its heading's hover, not under the card (the user, 2026-10-05)
					c.PartTips["SPEAR OPENER"] = "Signet of Might, then Disrupting Throw within 1 s of another Warrior's, then Maiming Spear, Spearmarshal's Support "
						"and Harrier's Toss in the 4 s after. The strip: each throw against the opener's middle one. The icons: struck through in red, left out; "
						"in grey, the signet was on cooldown. Hover an icon for when.";
					c.PartTips["MELEE BURST"] = "The first Bloodthirster after each weapon swap, against " + std::string(refWord) + "; on the call from 1 s before to 2.5 s after, between the lines.";
					out.push_back(c);
				}
			}
			into("chrono", "Chronomancer burst", {"Split Second", "Time Bomb"}, 7, true);
			into("ele", "Elementalist burst", {"Meteor", "Firestorm", "Volcano", "Fulgor"}, 6);
			// shroud 4: a core Necromancer's Life Transfer, a Reaper's Soul Spiral (the user, 2026-10-04)
			into("necro", "Necromancer burst", {"Life Transfer", "Soul Spiral"}, 8);
			// Winds of Disenchantment: one or two per spike (a cast outside our spikes can be area denial, not counted)
			{
				std::vector<Use> winds = CastsNamed(f, {"Winds of Disenchantment"});
				if (!winds.empty())
				{
					CallCard c;
					c.Key = "winds"; c.Group = 0; c.Name = "Winds of Disenchantment"; c.Should = "1 or 2 per spike"; c.Skill = winds[0].Skill;
					int spikes = 0, fine = 0, outside = 0;
					std::string stacked;
					for (int64_t t : f.OurSpikesMs)
					{
						std::vector<const Use*> in;
						auto [lo, hi] = SpikeWindow(f, true, t, 3000, 3000);
						for (const Use& u : winds) { if (u.Ms >= lo && u.Ms <= hi) { in.push_back(&u); } }
						if (in.empty()) { continue; }
						spikes++;
						fine += in.size() <= 2;
						if (in.size() > 2 && stacked.empty()) { stacked = std::to_string(in.size()) + " at once at " + Duration(t); }
						for (const Use* u : in)
						{
							CallRow r{u->By->Name, "Winds of Disenchantment", u->Ms, "", std::to_string(in.size()) + " in this spike", in.size() <= 2, 0};
							r.Section = "Spike at " + Duration(t);
							c.Rows.push_back(r);
						}
					}
					for (const Use& u : winds) { outside += !InSpike(f, true, u.Ms, 3000, 3000); }
					c.Num = fine; c.Den = spikes;
					c.Line = spikes ? "1 or 2 in " + std::to_string(fine) + " of " + std::to_string(spikes) + (spikes == 1 ? " spike with it" : " spikes with it") : std::string("none in ally spikes");
					c.Short = spikes ? "1 or 2 in " + std::to_string(fine) + " of " + std::to_string(spikes) : std::string("none in spikes");
					if (!stacked.empty()) { c.Line += " \xc2\xb7 " + stacked; }
					if (outside) { c.Line += " \xc2\xb7 " + std::to_string(outside) + " outside spikes"; }
					if (!stacked.empty()) { c.Verdict = "stacked"; c.Kind = 2; }
					else if (spikes) { c.Verdict = "spread"; c.Kind = 0; }
					else { c.Verdict = "not in spikes"; c.Kind = 1; }
					c.Detail = "Each cast in an ally spike, with how many were cast in that spike";
					out.push_back(c);
				}
			}
			// Spinal Shivers at the opening: together, one target
			{
				std::vector<Use> sh = CastsNamed(f, {"Spinal Shivers"});
				if (!sh.empty())
				{
					CallCard c;
					c.Key = "spinal"; c.Group = 0; c.Name = "Spinal Shivers"; c.Should = "opening, one target"; c.Skill = sh[0].Skill;
					std::set<const Player*> users, early;
					std::set<int> targets;
					int32_t first = INT32_MAX, last = INT32_MIN;
					for (const Use& u : sh)
					{
						users.insert(u.By);
						if (u.Ms > 8000 || early.count(u.By)) { continue; }
						early.insert(u.By);
						first = std::min(first, u.Ms); last = std::max(last, u.Ms);
						int target = -1;
						for (const auto& h : u.By->HitsOut) { if (h.Skill == u.Skill && h.Ms >= u.Ms && h.Ms <= u.Ms + 1500) { target = h.Enemy; break; } }
						if (target >= 0) { targets.insert(target); }
						c.Rows.push_back({u.By->Name, "Spinal Shivers", u.Ms, "opening", target >= 0 ? f.Enemies[target].Spec + " " + std::to_string(target + 1) : std::string("no hit"), true, 0});
					}
					c.Num = static_cast<int>(early.size()); c.Den = static_cast<int>(users.size());
					c.Line = std::to_string(early.size()) + " of " + std::to_string(users.size()) + " in the first 8 s";
					c.Short = std::to_string(early.size()) + " of " + std::to_string(users.size()) + " at the opening";
					if (!early.empty())
					{
						c.Line += " \xc2\xb7 on " + std::to_string(targets.size()) + (targets.size() == 1 ? " target" : " targets") + " \xc2\xb7 within " + Num((last - first) / 1000.0) + " s";
					}
					bool all = early.size() == users.size(), one = targets.size() <= 1, close = last - first <= 1000;
					c.Verdict = early.empty() ? "not at the opening" : all && one && close ? "together" : !one ? "split" : "spread out";
					c.Kind = early.empty() ? 2 : all && one && close ? 0 : 1;
					c.Detail = "Each player's first Spinal Shivers in the first 8 s, and whom it hit";
					out.push_back(c);
				}
			}
			// Battle Standard on a down (Warriors: to finish an enemy, or to revive ours)
			{
				std::vector<Use> bs = CastsNamed(f, {"Battle Standard"}, 2);
				if (!bs.empty())
				{
					CallCard c;
					c.Key = "standard"; c.Group = 0; c.Name = "Battle Standard"; c.Should = "on a down"; c.Skill = bs[0].Skill;
					int onEnemy = 0, onAlly = 0, finished = 0;
					for (const Use& u : bs)
					{
						const auto* me = NearestPos(*u.By, u.Ms);
						int e = 0, fin = 0, a = 0;
						for (const auto& en : f.Enemies)
						{
							for (size_t i = 0; me && i < en.DownSpans.size(); i++)
							{
								const auto& s = en.DownSpans[i];
								if (s.Dead || s.From > u.Ms + 2000 || s.To < u.Ms) { continue; }
								const auto* them = EnemyPos(en, u.Ms);
								if (!them || std::hypot(me->X - them->X, me->Y - them->Y) > 1200) { continue; }
								e++;
								fin += i + 1 < en.DownSpans.size() && en.DownSpans[i + 1].Dead && en.DownSpans[i + 1].From == s.To && s.To <= u.Ms + 4000;
							}
						}
						for (const Player& q : f.Players)
						{
							const auto* them = NearestPos(q, u.Ms);
							if (&q == u.By || !me || !them || std::hypot(me->X - them->X, me->Y - them->Y) > 1200) { continue; }
							for (const auto& s : q.DownSpans) { a += !s.Dead && s.From <= u.Ms + 2000 && s.To >= u.Ms; }
						}
						onEnemy += e > 0; onAlly += e == 0 && a > 0; finished += fin;
						std::string what = e ? std::to_string(e) + " enemy down, " + std::to_string(fin) + " finished" : a ? std::to_string(a) + (a == 1 ? " ally down" : " allies down") : std::string("nobody down");
						c.Rows.push_back({u.By->Name, "Battle Standard", u.Ms, "", what, e > 0 || a > 0, 0});
					}
					c.Num = onEnemy + onAlly; c.Den = static_cast<int>(bs.size());
					c.Line = std::to_string(onEnemy) + " on an enemy down (" + std::to_string(finished) + " finished) \xc2\xb7 " + std::to_string(onAlly) + " on allies \xc2\xb7 " +
						std::to_string(bs.size() - onEnemy - onAlly) + " on nobody";
					c.Short = std::to_string(onEnemy + onAlly) + " of " + std::to_string(bs.size()) + " on a down";
					Rate(c, double(onEnemy + onAlly) / bs.size(), "on a down", "partly", "on nobody");
					c.Detail = "Each cast: downed enemies or allies within 1200 of the caster";
					out.push_back(c);
				}
			}

			// ---- before their spikes ---------------------------------------------------------------------------------
			{
				std::vector<InvulnUse> tales;
				for (const InvulnUse& u : InvulnUses(f, 0, f.DurationMs)) { if (u.Skill == "Tale of the August Queen") { tales.push_back(u); } }
				if (!tales.empty())
				{
					CallCard c;
					c.Key = "tale"; c.Group = 1; c.Name = "Tale of the August Queen"; c.Should = "before enemy spikes"; c.Skill = kTaleOfTheAugustQueen;
					int caught = 0, early = 0;
					std::string firstEarly;
					// Enemy spikes are found in the damage that landed, and a Tale on time takes that damage: the spike it caught
					// may not be there at all. So each Tale also counts the enemy hits in its distortion, the ones it absorbed
					// with the ones that landed, against the hits a second of the round's enemy spikes (the user, 2026-10-05)
					const size_t secs = static_cast<size_t>(f.DurationMs / 1000 + 1);
					// enemy hits per ally per second, landed and absorbed (an invulnerability took them): [player][second]
					std::vector<std::vector<int>> hits(f.Players.size(), std::vector<int>(secs, 0)), absorbed(f.Players.size(), std::vector<int>(secs, 0));
					// (strikes only: condition ticks can't be absorbed)
					for (auto& [ms, i] : f.StrikesIn)
					{
						if (ms >= 0 && static_cast<size_t>(ms / 1000) < secs && i >= 0 && static_cast<size_t>(i) < f.Players.size()) { hits[i][ms / 1000]++; }
					}
					for (const auto& n : f.NegatedHits)
					{
						if (n.Kind != 0 || n.Ms < 0 || static_cast<size_t>(n.Ms / 1000) >= secs || n.Player < 0 || static_cast<size_t>(n.Player) >= f.Players.size()) { continue; }
						absorbed[n.Player][n.Ms / 1000]++;
						hits[n.Player][n.Ms / 1000]++;
					}
					// hits a second on each of these players over a window
					auto rate = [&](const std::vector<int>& aWho, int64_t aFrom, int64_t aTo, const std::vector<std::vector<int>>& aOf)
					{
						int n = 0, bins = 0;
						for (int64_t b2 = std::max<int64_t>(0, aFrom / 1000); b2 <= aTo / 1000 && b2 < static_cast<int64_t>(secs); b2++)
						{
							bins++;
							for (int i : aWho) { if (i >= 0 && static_cast<size_t>(i) < aOf.size()) { n += aOf[i][b2]; } }
						}
						return bins && !aWho.empty() ? double(n) / bins / aWho.size() : 0.0;
					};
					std::vector<int> everyone;
					for (size_t i = 0; i < f.Players.size(); i++) { everyone.push_back(static_cast<int>(i)); }
					std::vector<double> spikeRates;
					for (int64_t t : f.TheirSpikesMs) { spikeRates.push_back(rate(everyone, t - 1000, t + 1000, hits)); }
					std::sort(spikeRates.begin(), spikeRates.end());
					const double spikeRate = spikeRates.empty() ? 0 : spikeRates[spikeRates.size() / 2];
					for (const InvulnUse& u : tales)
					{
						int64_t catchAt = -1, after = -1;
						// a spike it caught: the spike's run overlaps its distortion (1 s slack either side); one it ran out before:
						// starting after it, within 6 s of the use
						for (int64_t t : f.TheirSpikesMs)
						{
							auto [from, to] = SpikeWindow(f, false, t, 0, 0);
							if (to >= u.Ms - 1000 && from <= u.Ms + u.Duration + 1000) { catchAt = t; }
							else if (from > u.Ms + u.Duration + 1000 && from <= u.Ms + 6000 && after < 0) { after = t; }
						}
						// a burst it took: on the allies it covered, enemy hits at an enemy spike's rate or more, most of them absorbed
						const double inTale = rate(u.To, u.Ms, u.Ms + u.Duration, hits), took = rate(u.To, u.Ms, u.Ms + u.Duration, absorbed);
						const bool burst = catchAt < 0 && spikeRate > 0 && inTale >= 0.6 * spikeRate && took * 2 >= inTale;
						const bool ok = catchAt >= 0 || burst;
						caught += ok;
						bool tooEarly = !ok && after >= 0;
						early += tooEarly;
						if (tooEarly && firstEarly.empty()) { firstEarly = Duration(u.Ms) + ", enemy spike " + Duration(after); }
						// a row of a table, not a sentence (the user, 2026-10-06: the card was full of text)
						auto num = [](double v) { char buf[16]; std::snprintf(buf, sizeof buf, "%.1f", v); return std::string(buf); };
						CallRow r;
						r.Who = f.Players[u.By].Name; r.Skill = "Tale of the August Queen"; r.Ms = u.Ms; r.Good = ok;
						CallCell when{Duration(u.Ms)}, by{f.Players[u.By].Name}, what, on{std::to_string(u.To.size())}, inHits{num(inTale)}, inTook{num(took)};
						on.Right = inHits.Right = inTook.Right = true;
						if (catchAt >= 0) { what = {"spike " + Duration(catchAt), "The enemy spike's peak at " + Duration(catchAt) + " fell while its distortion ran.", 0}; }
						else if (burst) { what = {"burst", "No enemy spike peak in it, but the allies it covered took enemy hits at a spike's rate or more, mostly absorbed.", 0}; }
						else if (tooEarly) { what = {"too early", "It ran out before the enemy spike at " + Duration(after) + ".", 2}; }
						else { what = {"none", "No enemy spike or burst while it ran.", 1}; }
						r.Table = {when, by, what, on, inHits, inTook};
						c.Rows.push_back(r);
					}
					c.Num = caught; c.Den = static_cast<int>(tales.size());
					c.Head = {"Used", "By", "Caught", "Allies", "Enemy hits /s each", "Absorbed /s each"};
					c.HeadTips = {"When it went off", "Who used it", "The spike or burst it took", "Allies it covered", "Enemy hits per covered ally",
						"Hits its distortion absorbed"};
					c.Line = std::to_string(caught) + " of " + std::to_string(tales.size()) + " caught an enemy spike or burst";
					if (spikeRate > 0)
					{
						char rateText[80];
						std::snprintf(rateText, sizeof rateText, " \xc2\xb7 in an enemy spike: %.1f enemy hits /s on each ally", spikeRate);
						c.Line += rateText;
					}
					c.Short = std::to_string(caught) + " of " + std::to_string(tales.size()) + " caught a spike";
					if (early) { c.Line += " \xc2\xb7 " + std::to_string(early) + " too early (" + firstEarly + ")"; }
					Rate(c, double(caught) / tales.size(), "on time", "partly", early ? "too early" : "missed");
					c.Detail = "Each Tale: whether an enemy spike's peak fell while its distortion ran. As a Tale on time takes the damage that would "
						"show the spike, it also counts the enemy hits on the allies it covered, absorbed or not: at an enemy spike's rate (hits a second "
						"on each ally) or more, mostly absorbed, it took a burst.";
					out.push_back(c);
				}
			}
			if (!f.TheirSpikesMs.empty())
			{
				// stability at each enemy spike, a column per subgroup: how many of its members had it at the peak, what it blocked,
				// who went down (the user, 2026-10-06: a list of givers' names told nothing; what matters is whether each
				// subgroup had it when the spike hit, and how heavy the spike was)
				CallCard c;
				c.Key = "stab"; c.Group = 1; c.Name = "Stability"; c.Should = "on every subgroup at enemy spikes"; c.Skill = -200 - Analysis::kStability;
				std::vector<int> groups;
				for (const Player& p : f.Players) { if (p.Subgroup > 0 && std::find(groups.begin(), groups.end(), p.Subgroup) == groups.end()) { groups.push_back(p.Subgroup); } }
				std::sort(groups.begin(), groups.end());
				c.Head = {"Spike", "Downed", "CC blocked"};
				c.HeadTips = {"The enemy spike's peak", "Allies downed in it", "CC stability took, of all", };
				for (int g : groups) { c.Head.push_back("sg " + std::to_string(g)); c.HeadTips.push_back("Members with stability then"); }
				auto has = [](const std::vector<std::pair<int32_t, int32_t>>& aSpans, int32_t aMs)
				{
					return std::any_of(aSpans.begin(), aSpans.end(), [&](const std::pair<int32_t, int32_t>& sp) { return sp.first <= aMs && sp.second >= aMs; });
				};
				auto names = [](const std::vector<std::string>& aNames)
				{
					std::string out;
					for (const std::string& n : aNames) { out += (out.empty() ? "" : ", ") + n; }
					return out;
				};
				int held = 0, allBlocked = 0, allLanded = 0;
				std::vector<std::string> gapAt;
				for (int64_t t64 : f.TheirSpikesMs)
				{
					const int32_t t = static_cast<int32_t>(t64);
					const auto [wFrom, wTo] = SpikeWindow(f, false, t64, 3000, 3000);
					const auto [gFrom, gTo] = SpikeWindow(f, false, t64, 3000, 0, true);
					const int32_t from = static_cast<int32_t>(wFrom), to = static_cast<int32_t>(wTo);
					CallRow r;
					r.Ms = t; r.Who = Duration(t);
					int downs = 0, blocked = 0, landed = 0;
					bool full = true, anyHit = false;
					std::vector<std::string> landedOn;
					std::vector<CallCell> cells;
					for (int g : groups)
					{
						std::vector<std::string> without, downed, ccOn, givers;
						int alive = 0, gBlocked = 0, gLanded = 0;
						bool hit = false;
						for (const Player& p : f.Players)
						{
							if (p.Subgroup != g) { continue; }
							for (const auto& sp : p.DownSpans) { if (!sp.Dead && SpikeOf(f, false, sp.From) == t64) { downed.push_back(p.Name); break; } }
							hit |= std::any_of(p.HitsIn.begin(), p.HitsIn.end(), [&](const auto& h) { return h.Ms >= from && h.Ms <= to; });
							for (auto& [ms, kind] : p.StabLost) { gBlocked += kind == 1 && ms >= from && ms <= to; }
							int cc = 0;
							for (const auto& h : p.CcIn) { cc += h.Ms >= from && h.Ms <= to; }
							if (cc) { gLanded += cc; ccOn.push_back(p.Name); }
							if (DownAt(p, t)) { continue; }
							alive++;
							if (!has(p.StabOnMe, t)) { without.push_back(p.Name); }
						}
						// who gave it to this subgroup in the 3 s before the peak
						for (const Player& q : f.Players)
						{
							for (const auto& gv : q.StabGives)
							{
								if (gv.SelfOnly || gv.Ms < gFrom || gv.Ms > gTo) { continue; }
								bool toGroup = std::any_of(gv.Targets.begin(), gv.Targets.end(), [&](int x) { return x >= 0 && static_cast<size_t>(x) < f.Players.size() && &f.Players[x] != &q && f.Players[x].Subgroup == g; });
								if (toGroup) { givers.push_back(q.Name); break; }
							}
						}
						downs += static_cast<int>(downed.size());
						blocked += gBlocked; landed += gLanded;
						landedOn.insert(landedOn.end(), ccOn.begin(), ccOn.end());
						const int with = alive - static_cast<int>(without.size());
						CallCell cell;
						cell.Text = alive ? std::to_string(with) + "/" + std::to_string(alive) : std::string("down");
						cell.Right = true;
						cell.Downs = static_cast<int>(downed.size());
						cell.Kind = !hit ? 3 : !alive ? 2 : without.empty() ? 0 : with * 2 >= alive ? 1 : 2;
						// a spike held: every subgroup it hit had stability on half its members or more (the red cells are the gaps;
						// one player without it is gold, not a failed spike)
						const bool gap = hit && (!alive || with * 2 < alive);
						if (hit) { anyHit = true; full &= !gap; }
						if (gap) { gapAt.push_back("sg " + std::to_string(g) + " at " + Duration(t)); }
						cell.Tip = "Subgroup " + std::to_string(g) + ", enemy spike at " + Duration(t) + (hit ? "" : ": the enemy didn't hit it") + "\n" +
							(alive ? std::to_string(with) + " of " + std::to_string(alive) + " with stability at the peak" + (without.empty() ? "" : "; without: " + names(without))
								: std::string("all of them were down at the peak"));
						if (gBlocked || gLanded)
						{
							cell.Tip += "\nCC: stability took " + std::to_string(gBlocked) + ", " + std::to_string(gLanded) + " landed" + (ccOn.empty() ? "" : " (on " + names(ccOn) + ")");
						}
						if (!downed.empty()) { cell.Tip += "\nDowned: " + names(downed); }
						cell.Tip += givers.empty() ? std::string("\nNobody gave them stability in the 3 s before") : "\nGiven in the 3 s before by " + names(givers);
						cells.push_back(cell);
					}
					held += anyHit && full;
					allBlocked += blocked; allLanded += landed;
					CallCell when{Duration(t)}, down, cc;
					when.Tip = "The enemy spike's peak; CC and stability are counted 3 s either side of it.";
					down.Right = true;
					down.Downs = downs;
					if (!downs) { down.Text = "0"; down.Kind = 3; }
					cc.Right = true;
					if (blocked + landed == 0) { cc.Text = "no CC"; cc.Kind = 3; }
					else
					{
						cc.Text = std::to_string(blocked) + " of " + std::to_string(blocked + landed);
						cc.Kind = blocked * 10 >= (blocked + landed) * 9 ? 0 : blocked * 4 >= (blocked + landed) * 3 ? 1 : 2;
						cc.Tip = "Stability took " + std::to_string(blocked) + " of the " + std::to_string(blocked + landed) + " CC on allies 3 s either side of the peak" +
							(landed ? "; it landed on " + names(landedOn) : std::string());
					}
					r.Good = anyHit && full;
					r.Table = {when, down, cc};
					r.Table.insert(r.Table.end(), cells.begin(), cells.end());
					c.Rows.push_back(r);
				}
				const int spikes = static_cast<int>(f.TheirSpikesMs.size());
				c.Num = held; c.Den = spikes;
				c.Line = "on half or more of every subgroup hit at " + std::to_string(held) + " of " + std::to_string(spikes) + " enemy spikes";
				if (allBlocked + allLanded) { c.Line += " \xc2\xb7 it took " + std::to_string(allBlocked) + " of " + std::to_string(allBlocked + allLanded) + " CC on allies"; }
				if (!gapAt.empty())
				{
					c.Line += " \xc2\xb7 under half: ";
					for (size_t i = 0; i < gapAt.size() && i < 3; i++) { c.Line += (i ? ", " : "") + gapAt[i]; }
					if (gapAt.size() > 3) { c.Line += " and " + std::to_string(gapAt.size() - 3) + " more"; }
				}
				c.Short = "held at " + std::to_string(held) + " of " + std::to_string(spikes) + " spikes";
				Rate(c, spikes ? double(held) / spikes : 0.0, "held", "partly", "short");
				c.Detail = "A row per enemy spike, a column per subgroup: of its members up at the spike's peak, how many had stability then. Blue: all; gold: half "
					"or more; red: under half; grey: the enemy didn't hit that subgroup in the spike. A spike held when no subgroup it hit was red. The down "
					"mark: allies downed in the spike. Hover a cell for who was without, the CC it took and who gave it.";
				out.push_back(c);
				// Druid heals before their spikes
				std::vector<Use> heals = CastsNamed(f, {"Glyph of Alignment", "Healing Spring"});
				if (!heals.empty())
				{
					CallCard h;
					h.Key = "heals"; h.Group = 1; h.Name = "Druid heals"; h.Should = "before enemy spikes"; h.Skill = heals[0].Skill;
					int before = 0;
					for (int64_t t : f.TheirSpikesMs)
					{
						std::vector<std::string> who;
						auto [lo, hi] = SpikeWindow(f, false, t, 3000, 0, true);
						for (const Use& u : heals) { if (u.Ms >= lo && u.Ms <= hi) { who.push_back(u.By->Name + " (" + Name(f, u.Skill) + ")"); } }
						before += !who.empty();
						std::string all;
						for (const std::string& n : who) { all += (all.empty() ? "" : ", ") + n; }
						h.Rows.push_back({who.empty() ? std::string("nobody") : std::to_string(who.size()) + (who.size() == 1 ? " cast" : " casts"), "", static_cast<int32_t>(t), "",
							who.empty() ? std::string("none in the 3 s before") : all, !who.empty(), 0});
					}
					h.Num = before; h.Den = static_cast<int>(f.TheirSpikesMs.size());
					h.Line = "Glyph of Alignment, Healing Spring before " + std::to_string(before) + " of " + std::to_string(f.TheirSpikesMs.size()) + " enemy spikes";
					h.Short = "before " + std::to_string(before) + " of " + std::to_string(f.TheirSpikesMs.size()) + " spikes";
					Rate(h, double(before) / f.TheirSpikesMs.size(), "on time", "partly", "late");
					h.Detail = "Each enemy spike: Glyph of Alignment or Healing Spring in the 3 s before its peak";
					out.push_back(h);
				}
			}

			// ---- after downs, and both ways ----------------------------------------------------------------------------
			{
				std::vector<Down> downs = Downs(f);
				if (!downs.empty())
				{
					CallCard c;
					c.Key = "revives"; c.Group = 2; c.Name = "Revives"; c.Should = "after downs"; c.Skill = -302;
					int up = 0;
					for (const Down& d : downs)
					{
						up += !d.Died;
						c.Rows.push_back({d.P->Name, "", d.S.From, "", d.Died ? std::string("died") : GotUpHow(f, d), !d.Died, 0});
					}
					int idle = 0;
					for (const Player& p : f.Players) { for (const auto& u : p.ReviveUses) { idle += u.Done && u.DownNear == 0; } }
					c.Num = up; c.Den = static_cast<int>(downs.size());
					c.Line = std::to_string(up) + " of " + std::to_string(downs.size()) + " downs got up";
					c.Short = idle ? std::to_string(idle) + " used with nobody down" : std::string("no revive skill wasted");
					if (idle) { c.Line += " \xc2\xb7 " + std::to_string(idle) + (idle == 1 ? " revive skill" : " revive skills") + " with nobody down"; }
					c.Verdict = std::to_string(up) + " of " + std::to_string(downs.size()) + " up";
					double share = double(up) / downs.size();
					c.Kind = share >= 0.6 ? 0 : share >= 0.35 ? 1 : 2;
					c.Detail = "Each down and how it ended";
					out.push_back(c);
				}
			}
			auto withSpike = [&](const char* aKey, const char* aName, const char* aSkill, int32_t aWindow, const char* aShould)
			{
				std::vector<Use> casts = CastsNamed(f, {aSkill});
				if (casts.empty()) { return; }
				CallCard c;
				c.Key = aKey; c.Group = 2; c.Name = aName; c.Should = aShould; c.Skill = casts[0].Skill;
				int on = 0;
				for (const Use& u : casts)
				{
					auto [ours, offO] = Nearest(f.OurSpikesMs, u.Ms);
					auto [theirs, offT] = Nearest(f.TheirSpikesMs, u.Ms);
					bool withOurs = ours >= 0 && offO >= -aWindow && offO <= 2000, withTheirs = theirs >= 0 && offT >= -aWindow && offT <= 2000;
					on += withOurs || withTheirs;
					std::string what = withOurs ? "ally spike at " + Duration(ours) : withTheirs ? "enemy spike at " + Duration(theirs) : std::string("no spike near");
					c.Rows.push_back({u.By->Name, aSkill, u.Ms, "", what, withOurs || withTheirs, 0});
				}
				c.Num = on; c.Den = static_cast<int>(casts.size());
				c.Line = std::to_string(on) + " of " + std::to_string(casts.size()) + " with a spike, ally or enemy";
				c.Short = std::to_string(on) + " of " + std::to_string(casts.size()) + " with a spike";
				Rate(c, double(on) / casts.size(), "with a spike", "partly", "off the call");
				c.Detail = "Each cast: the spike it went with";
				out.push_back(c);
			};
			withSpike("crescendo", "Crescendo", "Crescendo", 2000, "with the call");
			withSpike("split", "Continuum Split", "Continuum Split", 6000, "by what's cast in it");
			return out;
		}
	}

	const std::vector<CallCard>& RoundCalls(const Fight& f)
	{
		static std::string stamp;
		static std::vector<CallCard> cards;
		if (stamp != f.Stamp) { stamp = f.Stamp; cards = Build(f); }
		return cards;
	}
}
