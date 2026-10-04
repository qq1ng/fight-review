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
			std::vector<std::vector<Use>> calls;
			for (const Use& u : wells)
			{
				if (calls.empty() || u.Ms - calls.back().front().Ms > 3000) { calls.push_back({}); } // a call: wells within 3 s of its first (chained gaps merged waves into 12 s)
				calls.back().push_back(u);
			}
			std::vector<int64_t> wellAt;
			for (auto& c : calls) { wellAt.push_back(c[c.size() / 2].Ms); }
			const bool byWells = !wellAt.empty();
			const std::vector<int64_t>& ourRefs = byWells ? wellAt : f.OurSpikesMs;
			const char* refWord = byWells ? "the wells" : "our spikes";

			// ---- in our spikes ------------------------------------------------------------------------------------
			if (!wells.empty())
			{
				CallCard c;
				c.Key = "wells"; c.Group = 0; c.Name = "Wells"; c.Should = "together"; c.Skill = wells[0].Skill;
				int together = 0, downs = 0, strips = 0;
				std::set<std::pair<const Player*, int32_t>> rows;
				for (size_t k = 0; k < calls.size(); k++)
				{
					// a row per call: how many of each well, how many within 1 s of its middle, who was off (the user,
					// 2026-10-04: a row per well, both kinds mixed, was messy)
					std::map<std::string, int> kinds;
					std::vector<std::string> off;
					int in = 0, callDowns = 0;
					int32_t spread = calls[k].back().Ms - calls[k].front().Ms;
					for (const Use& u : calls[k])
					{
						bool ok = std::abs(static_cast<int32_t>(u.Ms - wellAt[k])) <= 1000;
						in += ok;
						kinds[Name(f, u.Skill)]++;
						if (!ok) { off.push_back(u.By->Name + " " + Offset(static_cast<int32_t>(u.Ms - wellAt[k]))); }
					}
					together += in;
					for (int32_t d : f.EnemyDownMs) { callDowns += d >= calls[k].front().Ms && d <= calls[k].back().Ms + 5000; }
					downs += callDowns;
					std::string what;
					for (auto& [n, cnt] : kinds) { what += (what.empty() ? "" : ", ") + std::to_string(cnt) + " " + n; }
					what += " \xc2\xb7 " + std::to_string(in) + " of " + std::to_string(calls[k].size()) + " within 1 s (spread " + Num(spread / 1000.0) + " s) \xc2\xb7 " +
						std::to_string(callDowns) + (callDowns == 1 ? " down" : " downs");
					if (!off.empty()) { what += " \xc2\xb7 off: " + Names(off, 3); }
					c.Rows.push_back({std::to_string(calls[k].size()) + (calls[k].size() == 1 ? " well" : " wells"), "", static_cast<int32_t>(wellAt[k]), "", what,
						in == static_cast<int>(calls[k].size()), 0});
				}
				for (const Player& p : f.Players) { for (auto& [sk, row] : p.Skills) { auto n = f.SkillNames.find(sk); if (n != f.SkillNames.end() && (n->second == "Well of Corruption" || n->second == "Well of Suffering")) { strips += row.Strips; } } }
				c.Num = together; c.Den = static_cast<int>(wells.size());
				c.Line = std::to_string(together) + " of " + std::to_string(wells.size()) + " together (within 1 s of their call)";
				c.Short = std::to_string(together) + " of " + std::to_string(wells.size()) + " together";
				if (strips) { c.Line += " \xc2\xb7 " + std::to_string(strips) + " boons stripped"; }
				c.Line += " \xc2\xb7 " + std::to_string(downs) + (downs == 1 ? " enemy down" : " enemy downs") + " within 5 s";
				Rate(c, double(together) / wells.size(), "together", "partly", "split");
				c.Detail = "Each call (wells within 3 s of its first): its wells, how many within 1 s of its middle cast, the enemy downs in the 5 s after";
				out.push_back(c);
			}
			// burst into the wells (or into our spikes, with no wells)
			auto into = [&](const char* aKey, const char* aName, std::vector<const char*> aSkills, uint32_t aProf)
			{
				std::vector<Use> casts = CastsNamed(f, aSkills, aProf);
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
					const int32_t mid = (kAfter - kBefore) / 2;
					if (it == best.end() || std::abs(off - mid) < std::abs(it->second.second - mid)) { best[key] = {&u, off}; }
				}
				for (auto& [key, v] : best)
				{
					const Use& u = *v.first;
					int32_t off = v.second;
					near++;
					bool ok = off >= -kBefore && off <= kAfter;
					on += ok;
					if (!ok && off > 0) { late++; lateNames.push_back(u.By->Name); }
					CallRow r{u.By->Name, Name(f, u.Skill), u.Ms, Duration(key.second), Offset(off), ok, off};
					r.Section = std::string(byWells ? "Wells at " : "Spike at ") + Duration(key.second);
					r.By = u.By;
					r.At = static_cast<int32_t>(key.second);
					c.Rows.push_back(r);
				}
				std::sort(c.Rows.begin(), c.Rows.end(), [](const CallRow& a, const CallRow& b) { return a.Ms - a.Offset != b.Ms - b.Offset ? a.Ms - a.Offset < b.Ms - b.Offset : a.Ms < b.Ms; });
				c.Num = on; c.Den = near;
				if (!near) { c.Line = std::to_string(casts.size()) + " cast, none within 5 s of " + refWord; c.Short = "none near " + std::string(refWord); c.Verdict = "off the call"; c.Kind = 2; }
				else
				{
					c.Line = std::to_string(on) + " of " + std::to_string(near) + " players' calls with " + refWord + " (1 s before to 2.5 s after)";
					c.Short = std::to_string(on) + " of " + std::to_string(near) + " on the call";
					if (!lateNames.empty()) { c.Line += " \xc2\xb7 late: " + Names(lateNames); }
					Rate(c, double(on) / near, "on the call", "partly", late * 2 >= near - on ? "late" : "early");
				}
				c.Detail = std::string("For each call (") + refWord + "), each player's best-timed cast within 5 s of it; on the call: 1 s before to 2.5 s after (the wells pulse after their cast)";
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
							std::vector<int32_t> sigs = castsOf(u->By, "Signet of Might");
							bool sig = std::any_of(sigs.begin(), sigs.end(), [&](int32_t ms) { return ms >= u->Ms - 3000 && ms <= u->Ms + 200; });
							int32_t last = INT32_MIN;
							for (int32_t ms : sigs) { if (ms < u->Ms - 3000) { last = std::max(last, ms); } }
							bool ready = last == INT32_MIN || u->Ms - last >= 16000;
							bool sync = std::any_of(dt.begin(), dt.end(), [&](const Use& o) { return o.By != u->By && std::abs(o.Ms - u->Ms) <= 1000; });
							// the follow-ups within 4 s
							std::string steps; // the follow-ups done, in order ("2 4 F1"); none: "-"
							int got = 0;
							for (auto [name, key] : {std::pair<const char*, const char*>{"Maiming Spear", "2"}, {"Spearmarshal's Support", "4"}, {"Harrier's Toss", "F1"}})
							{
								std::vector<int32_t> cs = castsOf(u->By, name);
								bool did = std::any_of(cs.begin(), cs.end(), [&](int32_t ms) { return ms >= u->Ms && ms <= u->Ms + 4000; });
								got += did;
								if (did) { steps += std::string(steps.empty() ? "" : " ") + key; }
							}
							if (steps.empty()) { steps = "-"; }
							bool sigOk = sig || !ready;
							withSig += sig; synced += sync; openerOk += sigOk && sync;
							sigReadyMissed += !sigOk;
							std::string what = std::string(sig ? "signet" : ready ? "NO SIGNET (ready)" : "signet on cd") + ", " + (sync ? "synced" : "alone") + "; then " + steps;
							CallRow r{u->By->Name, "Disrupting Throw", u->Ms, Duration(at), Offset(u->Ms - at) + "  " + what, sigOk && sync, u->Ms - at};
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
					c.Detail = "Spear opener: spear 3 with Signet of Might (when it was ready), synced with another Warrior (within 1 s); then: the spear 2, 4 and F1 cast in the 4 s after. "
						"Melee burst: the first Bloodthirster after each weapon swap, on the call from 1 s before to 2.5 s after " + std::string(refWord) + ".";
					out.push_back(c);
				}
			}
			into("chrono", "Chronomancer burst", {"Split Second", "Time Bomb"}, 7);
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
						for (const Use& u : winds) { if (u.Ms >= t - 3000 && u.Ms <= t + 3000) { in.push_back(&u); } }
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
					for (const Use& u : winds) { bool in = false; for (int64_t t : f.OurSpikesMs) { in |= u.Ms >= t - 3000 && u.Ms <= t + 3000; } outside += !in; }
					c.Num = fine; c.Den = spikes;
					c.Line = spikes ? "1 or 2 in " + std::to_string(fine) + " of " + std::to_string(spikes) + (spikes == 1 ? " spike with it" : " spikes with it") : std::string("none in our spikes");
					c.Short = spikes ? "1 or 2 in " + std::to_string(fine) + " of " + std::to_string(spikes) : std::string("none in spikes");
					if (!stacked.empty()) { c.Line += " \xc2\xb7 " + stacked; }
					if (outside) { c.Line += " \xc2\xb7 " + std::to_string(outside) + " outside spikes"; }
					if (!stacked.empty()) { c.Verdict = "stacked"; c.Kind = 2; }
					else if (spikes) { c.Verdict = "spread"; c.Kind = 0; }
					else { c.Verdict = "not in spikes"; c.Kind = 1; }
					c.Detail = "Each cast in one of our spikes, with how many were cast in that spike";
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
						std::string what = e ? std::to_string(e) + " enemy down, " + std::to_string(fin) + " finished" : a ? std::to_string(a) + " of ours down" : std::string("nobody down");
						c.Rows.push_back({u.By->Name, "Battle Standard", u.Ms, "", what, e > 0 || a > 0, 0});
					}
					c.Num = onEnemy + onAlly; c.Den = static_cast<int>(bs.size());
					c.Line = std::to_string(onEnemy) + " on an enemy down (" + std::to_string(finished) + " finished) \xc2\xb7 " + std::to_string(onAlly) + " on ours \xc2\xb7 " +
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
					c.Key = "tale"; c.Group = 1; c.Name = "Tale of the August Queen"; c.Should = "before their spikes"; c.Skill = kTaleOfTheAugustQueen;
					int caught = 0, early = 0;
					std::string firstEarly;
					for (const InvulnUse& u : tales)
					{
						int64_t catchAt = -1, after = -1;
						for (int64_t t : f.TheirSpikesMs)
						{
							if (t >= u.Ms - 1000 && t <= u.Ms + u.Duration + 1000) { catchAt = t; }
							else if (t > u.Ms + u.Duration + 1000 && t <= u.Ms + 6000 && after < 0) { after = t; }
						}
						caught += catchAt >= 0;
						bool tooEarly = catchAt < 0 && after >= 0;
						early += tooEarly;
						if (tooEarly && firstEarly.empty()) { firstEarly = Duration(u.Ms) + ", their spike " + Duration(after); }
						std::string what = catchAt >= 0 ? "caught their spike at " + Duration(catchAt) : tooEarly ? "ran out before their spike at " + Duration(after) : std::string("no enemy spike near");
						c.Rows.push_back({f.Players[u.By].Name, "Tale of the August Queen", u.Ms, "", what + ", on " + std::to_string(u.To.size()), catchAt >= 0, 0});
					}
					c.Num = caught; c.Den = static_cast<int>(tales.size());
					c.Line = std::to_string(caught) + " of " + std::to_string(tales.size()) + " caught an enemy spike";
					c.Short = std::to_string(caught) + " of " + std::to_string(tales.size()) + " caught a spike";
					if (early) { c.Line += " \xc2\xb7 " + std::to_string(early) + " too early (" + firstEarly + ")"; }
					Rate(c, double(caught) / tales.size(), "on time", "partly", early ? "too early" : "missed");
					c.Detail = "Each Tale: whether an enemy spike's peak fell while its distortion ran";
					out.push_back(c);
				}
			}
			if (!f.TheirSpikesMs.empty())
			{
				// stability that reached someone else in the 3 s before each enemy peak
				CallCard c;
				c.Key = "stab"; c.Group = 1; c.Name = "Stability"; c.Should = "before their spikes"; c.Skill = -200 - Analysis::kStability;
				int covered = 0;
				bool any = false;
				for (int64_t t : f.TheirSpikesMs)
				{
					std::vector<std::string> givers;
					for (const Player& p : f.Players)
					{
						for (const auto& g : p.StabGives)
						{
							if (g.SelfOnly || g.Ms < t - 3000 || g.Ms > t) { continue; }
							if (std::any_of(g.Targets.begin(), g.Targets.end(), [&](int x) { return &f.Players[x] != &p; })) { givers.push_back(p.Name); any = true; break; }
						}
					}
					covered += !givers.empty();
					c.Rows.push_back({givers.empty() ? std::string("nobody") : Names(givers, 4), "stability", static_cast<int32_t>(t), "", givers.empty() ? "none in the 3 s before" : "given in the 3 s before", !givers.empty(), 0});
				}
				if (any)
				{
					c.Num = covered; c.Den = static_cast<int>(f.TheirSpikesMs.size());
					c.Line = "given before " + std::to_string(covered) + " of " + std::to_string(f.TheirSpikesMs.size()) + " enemy spikes";
					c.Short = "before " + std::to_string(covered) + " of " + std::to_string(f.TheirSpikesMs.size()) + " spikes";
					Rate(c, double(covered) / f.TheirSpikesMs.size(), "on before", "partly", "late");
					c.Detail = "Each enemy spike: who gave stability to others in the 3 s before its peak";
					out.push_back(c);
				}
				// Druid heals before their spikes
				std::vector<Use> heals = CastsNamed(f, {"Glyph of Alignment", "Healing Spring"});
				if (!heals.empty())
				{
					CallCard h;
					h.Key = "heals"; h.Group = 1; h.Name = "Druid heals"; h.Should = "before their spikes"; h.Skill = heals[0].Skill;
					int before = 0;
					for (int64_t t : f.TheirSpikesMs)
					{
						std::vector<std::string> who;
						for (const Use& u : heals) { if (u.Ms >= t - 3000 && u.Ms <= t) { who.push_back(u.By->Name + " (" + Name(f, u.Skill) + ")"); } }
						before += !who.empty();
						h.Rows.push_back({who.empty() ? std::string("nobody") : Names(who, 3), "", static_cast<int32_t>(t), "", who.empty() ? "none in the 3 s before" : "cast in the 3 s before", !who.empty(), 0});
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
					std::string what = withOurs ? "our spike at " + Duration(ours) : withTheirs ? "their spike at " + Duration(theirs) : std::string("no spike near");
					c.Rows.push_back({u.By->Name, aSkill, u.Ms, "", what, withOurs || withTheirs, 0});
				}
				c.Num = on; c.Den = static_cast<int>(casts.size());
				c.Line = std::to_string(on) + " of " + std::to_string(casts.size()) + " with a spike, ours or theirs";
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
