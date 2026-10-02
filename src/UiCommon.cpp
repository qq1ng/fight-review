#include "UiCommon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "Icons.h"
#include "SkillIcons.h"
#include "imgui/imgui_internal.h"

namespace Ui
{
	// Blue = you / us, orange = the enemy, grey = the compared player (checked with the dataviz skill's validator
	// on the #141619 window surface: blue/orange pass all-pairs, the grey steps clear the normal-vision floor)
	const ImVec4 kMuted(0.54f, 0.53f, 0.51f, 1.0f);
	const ImU32 kYou = IM_COL32(0x39, 0x87, 0xe5, 255);
	const ImU32 kPeer = IM_COL32(0x6b, 0x70, 0x78, 255);
	const ImU32 kPeerTick = IM_COL32(0xa8, 0xad, 0xb3, 255);
	const ImU32 kEnemy = IM_COL32(0xd9, 0x59, 0x26, 255);
	const ImU32 kTrack = IM_COL32(0x2c, 0x2f, 0x35, 255);
	const ImU32 kLaneBg = IM_COL32(0x10, 0x11, 0x14, 255);
	const ImU32 kOurBand = IM_COL32(57, 135, 229, 60);
	const ImU32 kEnemyBand = IM_COL32(217, 89, 38, 70);
	const ImU32 kEnemyPre = IM_COL32(217, 89, 38, 30);

	State& S()
	{
		static State s;
		return s;
	}

	// ---- formatting -----------------------------------------------------------------------------------------------

	// 400123 -> 400k, 7833 -> 7.8k, 999 -> 999, 12.34 -> 12.3, 0.5 -> 0.50, whole numbers stay whole
	std::string Num(double aValue)
	{
		char buf[32];
		double a = std::fabs(aValue);
		if (a >= 1e6) { std::snprintf(buf, sizeof(buf), "%.2fM", aValue / 1e6); }
		else if (a >= 1e5) { std::snprintf(buf, sizeof(buf), "%.0fk", aValue / 1e3); }
		else if (a >= 1e3) { std::snprintf(buf, sizeof(buf), "%.1fk", aValue / 1e3); }
		else if (a >= 100 || aValue == std::floor(aValue)) { std::snprintf(buf, sizeof(buf), "%.0f", aValue); }
		else if (a >= 10) { std::snprintf(buf, sizeof(buf), "%.1f", aValue); }
		else { std::snprintf(buf, sizeof(buf), "%.2f", aValue); }
		return buf;
	}

	std::string Lower(std::string aText)
	{
		for (char& ch : aText) { if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch - 'A' + 'a'); } }
		return aText;
	}

	std::string Clock(const std::string& aStamp) // yyyymmdd-hhmmss -> hh:mm
	{
		return aStamp.size() >= 13 ? aStamp.substr(9, 2) + ":" + aStamp.substr(11, 2) : aStamp;
	}

	std::string Duration(int64_t aMs)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(aMs / 60000), static_cast<long long>(aMs / 1000 % 60));
		return buf;
	}

	double PerS(const Player& aPlayer, double aValue)
	{
		return aPlayer.ActiveMs > 0 ? aValue * 1000.0 / aPlayer.ActiveMs : 0.0;
	}

	// "62% (48 of 77)", or "-" without enough evidence
	std::string Share(int aPart, int aWhole, int aMinimum)
	{
		if (aWhole < aMinimum || aWhole <= 0) { return "-"; }
		return std::to_string(int(100.0 * aPart / aWhole + 0.5)) + "% (" + std::to_string(aPart) + " of " + std::to_string(aWhole) + ")";
	}

	ImVec4 ProfessionColor(uint32_t aProf)
	{
		static const ImVec4 kColors[] = {ImVec4(0.8f, 0.8f, 0.8f, 1), ImVec4(0.45f, 0.76f, 0.85f, 1),
			ImVec4(1.0f, 0.82f, 0.40f, 1), ImVec4(0.82f, 0.61f, 0.35f, 1), ImVec4(0.55f, 0.86f, 0.51f, 1),
			ImVec4(0.75f, 0.56f, 0.58f, 1), ImVec4(0.96f, 0.54f, 0.53f, 1), ImVec4(0.71f, 0.47f, 0.84f, 1),
			ImVec4(0.32f, 0.65f, 0.44f, 1), ImVec4(0.82f, 0.43f, 0.35f, 1)};
		return aProf < 10 ? kColors[aProf] : kColors[0];
	}

	// ---- metrics --------------------------------------------------------------------------------------------------

	const std::vector<Metric>& Metrics()
	{
		static std::vector<Metric> m = []
		{
			std::vector<Metric> v;
			v.push_back({"Healing", K_Amount, [](const Player& p) { return double(p.Heal); }, [](const SkillRow& r) { return double(r.Heal); }, true});
			v.push_back({"Barrier", K_Amount, [](const Player& p) { return double(p.Barrier); }, [](const SkillRow& r) { return double(r.Barrier); }, true});
			v.push_back({"Damage to players", K_Amount, [](const Player& p) { return double(p.Damage); }, [](const SkillRow& r) { return double(r.Damage); }});
			v.push_back({"Damage to all", K_Amount, [](const Player& p) { return double(p.DamageAll); }, [](const SkillRow& r) { return double(r.DamageAll); }});
			v.push_back({"Strips", K_Count, [](const Player& p) { return double(p.Strips); }, [](const SkillRow& r) { return double(r.Strips); }});
			v.push_back({"Cleanses", K_Count, [](const Player& p) { return double(p.Cleanses); }, [](const SkillRow& r) { return double(r.Cleanses); }});
			for (int b = 0; b < Analysis::kBoons; b++)
			{
				v.push_back({std::string(Analysis::kBoonNames[b]) + " to subgroup", K_Boon, [b](const Player& p) { return p.BoonGroupS[b]; },
					[b](const SkillRow& r) { return r.BoonGroupS[b]; }, false, b == 0 || b == Analysis::kStability});
			}
			for (int b = 0; b < Analysis::kBoons; b++)
			{
				v.push_back({std::string(Analysis::kBoonNames[b]) + " to squad", K_Boon, [b](const Player& p) { return p.BoonSquadS[b]; },
					[b](const SkillRow& r) { return r.BoonSquadS[b]; }, false, b == 0 || b == Analysis::kStability});
			}
			return v;
		}();
		return m;
	}

	bool Totals(const Player& p) { return p.Rounds <= 1; }

	// Boons: seconds given per second alive = allies kept covered (queued boons) or stacks kept on others (intensity)
	const char* RateUnit(const Metric& m, const Player& p)
	{
		switch (m.What)
		{
		case K_Amount: return Totals(p) ? "" : "/s";
		case K_Count: return Totals(p) ? "" : "/min";
		default: return m.Intensity ? "stacks" : "% uptime";
		}
	}

	double Rate(const Metric& m, const Player& p, double aValue)
	{
		// boons to the subgroup (summed players): stacks, or % uptime, over the rounds' length, as TopStats
		if (m.What == K_Boon && p.FightMs > 0) { return aValue * 1000.0 / p.FightMs * (m.Intensity ? 1.0 : 100.0); }
		if (m.What != K_Boon && Totals(p)) { return aValue; }
		return m.What == K_Count ? PerS(p, aValue) * 60.0 : PerS(p, aValue);
	}

	double CountRate(const Player& p, double aCount) { return Totals(p) ? aCount : PerS(p, aCount) * 60.0; }

	std::string WithUnit(const std::string& aNumber, const char* aUnit)
	{
		return aUnit && aUnit[0] ? aNumber + " " + aUnit : aNumber;
	}

	std::string ShownName(const std::string& aMeasure, bool aOneRound)
	{
		if (!aOneRound) { return aMeasure; }
		for (const char* unit : {" /min", " /s"})
		{
			size_t n = std::strlen(unit);
			if (aMeasure.size() > n && aMeasure.compare(aMeasure.size() - n, n, unit) == 0) { return aMeasure.substr(0, aMeasure.size() - n); }
		}
		return aMeasure;
	}

	bool Known(const Metric& m, const Player& p) { return !m.NeedsHealing || p.HealKnown; }

	int TimingWindow(int aMetric)
	{
		if (aMetric == kMetricHeal || aMetric == kMetricBarrier || aMetric == kMetricCleanses) { return Analysis::T_AnsweringTheirs; }
		if (aMetric >= kMetricGroupBoon) { return Analysis::T_AheadOfTheirs; }
		return Analysis::T_IntoOurs;
	}

	const char* WindowLabel(int aWindow)
	{
		static const char* kLabels[] = {"in our spike", "before an enemy spike", "just after an enemy spike"};
		return kLabels[aWindow];
	}

	// ---- players --------------------------------------------------------------------------------------------------

	// A player summed over several rounds (same account and spec)
	Player Sum(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec,
		std::map<int32_t, std::string>& aNames)
	{
		Player out;
		bool first = true;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.Account != aAccount || p.Spec != aSpec) { continue; }
				if (first)
				{
					out.Name = p.Name; out.Account = p.Account; out.Spec = p.Spec; out.Subgroup = p.Subgroup;
					out.ProfId = p.ProfId; out.EliteId = p.EliteId; first = false;
				}
				out.Pov |= p.Pov;
				out.HealKnown |= p.HealKnown;
				out.Rounds = static_cast<int>(aFights.size()); // the scope, not the rounds played: one rule for everyone compared
				out.HealLate += p.HealLate; out.PetsTook += p.PetsTook;
				if (out.HealLateMs < 0) { out.HealLateMs = p.HealLateMs; }
				out.ActiveMs += p.ActiveMs;
				out.FightMs += f->DurationMs;
				// boons to the subgroup as TopStats' group generation: per other member of the subgroup (scaled per
				// round, subgroups change size), over the fight's length (Rate), so Compare and You show one number
				auto gs = f->GroupSize.find(p.Subgroup);
				double perMember = gs != f->GroupSize.end() && gs->second >= 2 ? 1.0 / (gs->second - 1) : 0.0;
				out.Heal += p.Heal; out.HealDowned += p.HealDowned; out.Barrier += p.Barrier; out.Damage += p.Damage;
				out.DamageAll += p.DamageAll; out.Strips += p.Strips; out.Cleanses += p.Cleanses;
				out.Evades += p.Evades; out.Blocks += p.Blocks; out.Invulns += p.Invulns; out.Downs += p.Downs;
				out.Deaths += p.Deaths; out.CcTaken += p.CcTaken; out.CcTakenMs += p.CcTakenMs; out.CcNoStab += p.CcNoStab;
				out.DamageTaken += p.DamageTaken; out.HealToSelf += p.HealToSelf; out.HealToGroup += p.HealToGroup;
				out.HealToOthers += p.HealToOthers; out.DownedMs += p.DownedMs;
				out.DownSpans.insert(out.DownSpans.end(), p.DownSpans.begin(), p.DownSpans.end());
				out.StabEligible += p.StabEligible; out.StabCovered += p.StabCovered; out.StabReady += p.StabReady;
				out.StabAllyNominalMs += p.StabAllyNominalMs; out.StabRedundantMs += p.StabRedundantMs; out.StabSpikes += p.StabSpikes; out.StabSpikeShare += p.StabSpikeShare;
				out.StabAllyMs += p.StabAllyMs; out.StabSelfMs += p.StabSelfMs;
				out.CcDealt += p.CcDealt; out.Legends |= p.Legends; out.DownContribution += p.DownContribution;
				out.ReviveUses.insert(out.ReviveUses.end(), p.ReviveUses.begin(), p.ReviveUses.end());
				for (int b = 0; b < Analysis::kBoons; b++) { out.BoonGroupS[b] += p.BoonGroupS[b] * perMember; out.BoonSquadS[b] += p.BoonSquadS[b]; }
				for (auto& [skill, row] : p.Skills)
				{
					auto& o = out.Skills[skill];
					o.Casts += row.Casts; o.Hits += row.Hits; o.Heal += row.Heal; o.Barrier += row.Barrier; o.Damage += row.Damage;
					o.DamageAll += row.DamageAll; o.Strips += row.Strips; o.Cleanses += row.Cleanses;
					o.Interrupted += row.Interrupted; o.Cancelled += row.Cancelled;
					for (int b = 0; b < Analysis::kBoons; b++) { o.BoonGroupS[b] += row.BoonGroupS[b] * perMember; o.BoonSquadS[b] += row.BoonSquadS[b]; }
					for (int k = 0; k < Analysis::T_Count; k++) { o.Timing[k] += row.Timing[k]; }
				}
			}
			for (auto& [id, name] : f->SkillNames) { aNames.emplace(id, name); }
		}
		// one row per skill name across the rounds too (a trait-changed id in one round, the other id in another), within
		// the profession as the analysis does: the lowest id of the name in its players' rows
		std::map<std::string, int32_t> firstId;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.ProfId != out.ProfId) { continue; }
				for (auto& [id, row] : p.Skills)
				{
					auto n = aNames.find(id);
					if (id <= 0 || n == aNames.end() || n->second.empty() || std::to_string(id) == n->second) { continue; }
					auto it = firstId.emplace(n->second, id).first;
					it->second = std::min(it->second, id);
				}
			}
		}
		std::vector<int32_t> moved;
		for (auto& [id, row] : out.Skills)
		{
			auto n = aNames.find(id);
			if (id > 0 && n != aNames.end() && firstId.count(n->second) && firstId[n->second] != id) { moved.push_back(id); }
		}
		for (int32_t id : moved)
		{
			SkillRow from = out.Skills[id];
			out.Skills.erase(id);
			SkillRow& o = out.Skills[firstId[aNames[id]]];
			o.Casts += from.Casts; o.Hits += from.Hits; o.Heal += from.Heal; o.Barrier += from.Barrier; o.Damage += from.Damage;
			o.DamageAll += from.DamageAll; o.Strips += from.Strips; o.Cleanses += from.Cleanses;
			o.Interrupted += from.Interrupted; o.Cancelled += from.Cancelled;
			for (int b = 0; b < Analysis::kBoons; b++) { o.BoonGroupS[b] += from.BoonGroupS[b]; o.BoonSquadS[b] += from.BoonSquadS[b]; }
			for (int k = 0; k < Analysis::T_Count; k++) { o.Timing[k] += from.Timing[k]; }
		}
		return out;
	}

	// Group generation over several rounds: each round's value weighted by its length
	double GroupGenOver(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec, int aBoon)
	{
		double weighted = 0, total = 0;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.Account != aAccount || p.Spec != aSpec) { continue; }
				weighted += f->GroupGeneration(p, aBoon) * f->DurationMs;
				total += double(f->DurationMs);
			}
		}
		return total > 0 ? weighted / total : 0.0;
	}

	// Stability first (it is the support's job even when they also heal), then healing, else damage
	Role RoleOf(const Player& p)
	{
		if (p.ActiveMs > 0 && p.StabAllyMs >= 2 * p.ActiveMs) { return R_Stab; }
		if (p.HealKnown && p.Heal > p.Damage) { return R_Heal; }
		return R_Damage;
	}

	int CastsCutShort(const Player& p)
	{
		int n = 0;
		for (auto& [s, r] : p.Skills) { n += r.Interrupted + r.Cancelled; }
		return n;
	}

	int64_t SpikeOf(const std::vector<int64_t>& aPeaks, int64_t aMs)
	{
		int64_t best = -1;
		for (int64_t t : aPeaks)
		{
			if (aMs < t - kSpikeBeforeMs || aMs > t + kSpikeAfterMs) { continue; }
			if (best < 0 || std::llabs(aMs - t) < std::llabs(aMs - best)) { best = t; }
		}
		return best;
	}

	int64_t DeadMs(const Player& p)
	{
		int64_t ms = 0;
		for (const Analysis::Span& s : p.DownSpans) { if (s.Dead) { ms += s.To - s.From; } }
		return ms;
	}

	const SkillRow* Row(const Player& p, int32_t aSkill)
	{
		auto it = p.Skills.find(aSkill);
		return it == p.Skills.end() ? nullptr : &it->second;
	}

	double SpikeShare(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec)
	{
		double in = 0, all = 0;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.Account != aAccount || p.Spec != aSpec) { continue; }
				for (size_t s = 0; s < p.DamagePerS.size(); s++)
				{
					all += p.DamagePerS[s];
					int64_t mid = static_cast<int64_t>(s) * 1000 + 500;
					for (int64_t t : f->OurSpikesMs) { if (mid >= t - 2000 && mid <= t + 2000) { in += p.DamagePerS[s]; break; } }
				}
			}
		}
		return all > 0 ? 100.0 * in / all : -1.0;
	}

	double HealInEnemySpikes(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec)
	{
		double in = 0, all = 0;
		bool known = false;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.Account != aAccount || p.Spec != aSpec || !p.HealKnown) { continue; }
				if (p.HealLate * 2 > p.Heal) { continue; } // most of it arrived late, at one moment: its timing isn't known
				known = true;
				for (size_t s = 0; s < p.HealPerS.size(); s++)
				{
					all += p.HealPerS[s];
					int64_t mid = static_cast<int64_t>(s) * 1000 + 500;
					for (int64_t t : f->TheirSpikesMs) { if (mid >= t && mid <= t + 3000) { in += p.HealPerS[s]; break; } }
				}
			}
		}
		return known && all > 0 ? 100.0 * in / all : -1.0;
	}

	std::pair<int, int> CastsOnEnemySpikes(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec, int32_t aSkill)
	{
		int timed = 0, all = 0;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.Account != aAccount || p.Spec != aSpec) { continue; }
				auto it = p.Skills.find(aSkill);
				if (it == p.Skills.end()) { continue; }
				for (int32_t ms : it->second.CastMs)
				{
					all++;
					for (int64_t t : f->TheirSpikesMs) { if (ms >= t - 3000 && ms <= t + 1000) { timed++; break; } }
				}
			}
		}
		return {timed, all};
	}

	std::pair<int, int> RoundsWithRevive(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec)
	{
		int with = 0, played = 0;
		for (const FightPtr& f : aFights)
		{
			for (const Player& p : f->Players)
			{
				if (p.Account != aAccount || p.Spec != aSpec || p.ActiveMs < 30000) { continue; }
				played++;
				with += std::any_of(p.ReviveUses.begin(), p.ReviveUses.end(), [](const auto& u) { return u.Done && u.DownNear > 0; });
			}
		}
		return {with, played};
	}

	std::string Ctx::Name(int32_t aSkill) const
	{
		auto it = Names.find(aSkill);
		return it != Names.end() ? it->second : std::to_string(aSkill);
	}

	std::string Ctx::PeerLabel() const
	{
		if (SameSpec) { return You.Spec; }
		return MyRole == R_Heal ? "healer" : MyRole == R_Stab ? "stability support" : MyRole == R_Strip ? "stripper" : "damage player";
	}

	namespace
	{
#include "SpecJobs.inc"

		// What the log shows of a player's build over these rounds: Revenant legends (those in half their rounds or
		// more), and damage and stability or quickness against each round's squad average (the median over the rounds
		// they played 30 s or more)
		// Aid: the most the player gave of cleanses, healing (when known) or a boon, against the squad average
		struct BuildSigns { uint32_t Legends = 0; double Damage = 0, Support = 0, Aid = 0; bool Known = false; };

		BuildSigns SignsOf(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec)
		{
			BuildSigns b;
			std::vector<double> damage, support, aid;
			std::map<uint32_t, int> legendRounds; // a legend is the build when it shows in half their rounds or more
			int roundsWithLegends = 0;              // (one swapped in for a round or two isn't)
			for (const FightPtr& f : aFights)
			{
				const Player* me = nullptr;
				double dmg = 0, stab = 0, quick = 0, cleanses = 0, heal = 0;
				std::array<double, Analysis::kBoons> boons{};
				int n = 0, healers = 0;
				for (const Player& p : f->Players)
				{
					if (p.Account == aAccount && p.Spec == aSpec)
					{
						me = &p;
						if (p.Legends) { roundsWithLegends++; }
						for (uint32_t bit = 1; bit <= Analysis::L_Entity; bit <<= 1) { if (p.Legends & bit) { legendRounds[bit]++; } }
					}
					if (p.ActiveMs < 30000) { continue; }
					dmg += PerS(p, double(p.Damage));
					stab += f->SquadGeneration(p, Analysis::kStability);
					quick += f->SquadGeneration(p, 2);
					cleanses += PerS(p, p.Cleanses);
					if (p.HealKnown) { heal += PerS(p, double(p.Heal)); healers++; }
					for (int k = 0; k < Analysis::kBoons; k++) { boons[k] += f->SquadGeneration(p, k); }
					n++;
				}
				if (!me || me->ActiveMs < 30000 || n < 5) { continue; }
				dmg /= n; stab /= n; quick /= n;
				if (dmg > 0) { damage.push_back(PerS(*me, double(me->Damage)) / dmg); }
				support.push_back(std::max(stab > 0 ? f->SquadGeneration(*me, Analysis::kStability) / stab : 0.0,
					quick > 0 ? f->SquadGeneration(*me, 2) / quick : 0.0));
				double a = cleanses > 0 ? PerS(*me, me->Cleanses) / (cleanses / n) : 0.0;
				if (me->HealKnown && heal > 0) { a = std::max(a, PerS(*me, double(me->Heal)) / (heal / healers)); }
				for (int k = 0; k < Analysis::kBoons; k++) { if (boons[k] > 0) { a = std::max(a, f->SquadGeneration(*me, k) / (boons[k] / n)); } }
				aid.push_back(a);
			}
			auto median = [](std::vector<double> v)
			{
				if (v.empty()) { return 0.0; }
				std::sort(v.begin(), v.end());
				return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
			};
			for (auto& [bit, rounds] : legendRounds) { if (rounds * 2 >= roundsWithLegends) { b.Legends |= bit; } }
			b.Damage = median(damage);
			b.Known = !damage.empty();
			b.Aid = median(aid);
			b.Support = median(support);
			return b;
		}

		// A spec's CC landed per minute against each round's squad average, median over its player-rounds (30 s+)
		double SpecCcRatio(const std::vector<FightPtr>& aFights, const std::string& aSpec)
		{
			std::vector<double> ratios;
			for (const FightPtr& f : aFights)
			{
				double sum = 0;
				int n = 0;
				for (const Player& p : f->Players) { if (p.ActiveMs >= 30000) { sum += p.CcDealt * 60000.0 / p.ActiveMs; n++; } }
				if (n < 5 || sum <= 0) { continue; }
				for (const Player& p : f->Players)
				{
					if (p.Spec == aSpec && p.ActiveMs >= 30000) { ratios.push_back(p.CcDealt * 60000.0 / p.ActiveMs / (sum / n)); }
				}
			}
			if (ratios.size() < 3) { return 0; }
			std::sort(ratios.begin(), ratios.end());
			return ratios[ratios.size() / 2];
		}

		// Does a SpecJobs row's condition hold? (the conditions are listed in SpecJobs.inc)
		bool Holds(const std::string& aWhen, const BuildSigns& b)
		{
			if (aWhen.empty()) { return true; }
			if (aWhen == "Ventari") { return (b.Legends & Analysis::L_Centaur) != 0; }
			if (aWhen == "not Ventari") { return (b.Legends & Analysis::L_Centaur) == 0; }
			if (aWhen == "Mallyx") { return (b.Legends & Analysis::L_Demon) != 0; }
			if (aWhen == "Jalis") { return (b.Legends & Analysis::L_Dwarf) != 0; }
			if (aWhen == "Wanderer's") { return b.Damage >= 0.4; }
			if (aWhen == "damage build") { return b.Damage >= 1.5 && b.Support < 1.5; }
			if (aWhen == "hybrid build") { return b.Damage >= 1.5 && b.Support >= 1.5; }
			if (aWhen == "support build") { return b.Damage < 1.5; }
			// support: little damage and clearly giving something (1.5x the squad average of cleanses, healing or a boon);
			// little damage and nothing given is a player who spent the rounds dead or away, judged as the power build
			const bool support = b.Known && b.Damage < 0.6 && b.Aid >= 1.5;
			if (aWhen == "power") { return !support; }
			if (aWhen == "support") { return support; }
			return false;
		}
	}

	std::vector<std::string> JobsFor(const std::vector<FightPtr>& aFights, const std::string& aAccount, const std::string& aSpec, std::string* aBuild)
	{
		// Asked for every player on your spec when a context is built: keep the answers until the rounds change
		struct Entry { std::vector<std::string> Jobs; std::string Build; };
		static std::map<std::string, Entry> memo;
		static uint64_t memoVersion = UINT64_MAX;
		if (aFights.empty()) { return {}; }
		if (memoVersion != DataVersion()) { memo.clear(); memoVersion = DataVersion(); }
		const std::string key = aAccount + "|" + aSpec;
		auto it = memo.find(key);
		if (it == memo.end())
		{
			Entry e;
			BuildSigns signs;
			bool haveSigns = false;
			std::vector<std::string> builds;
			for (const SpecJobs& row : kSpecJobs)
			{
				if (aSpec != row.Spec) { continue; }
				const std::string when = row.When;
				if (!when.empty() && !haveSigns) { signs = SignsOf(aFights, aAccount, aSpec); haveSigns = true; }
				if (!Holds(when, signs)) { continue; }
				for (const char* job : row.Jobs) { if (std::find(e.Jobs.begin(), e.Jobs.end(), job) == e.Jobs.end()) { e.Jobs.push_back(job); } }
				if (!when.empty() && when != "not Ventari" && std::find(builds.begin(), builds.end(), when) == builds.end()) { builds.push_back(when); }
			}
			// Outgoing CC as a job when this spec lands clearly more than the squad does tonight (the user, 2026-09-25:
			// important for a few classes; on all logs to date only Chronomancers do, at 1.8x the squad average)
			const char* kCc = "CC on enemies /min";
			if (std::find(e.Jobs.begin(), e.Jobs.end(), kCc) == e.Jobs.end() && SpecCcRatio(aFights, aSpec) >= 1.3) { e.Jobs.push_back(kCc); }
			if (!builds.empty())
			{
				bool legends = builds[0] == "Ventari" || builds[0] == "Mallyx" || builds[0] == "Jalis";
				std::string list;
				for (size_t i = 0; i < builds.size(); i++) { list += (i == 0 ? "" : i + 1 == builds.size() ? " and " : ", ") + builds[i]; }
				e.Build = legends ? "with " + list : builds[0] == "Wanderer's" ? "in Wanderer's gear" : builds[0] == "power" || builds[0] == "support" ? "on a " + builds[0] + " build" : "on a " + list;
			}
			it = memo.emplace(key, std::move(e)).first;
		}
		if (aBuild) { *aBuild = it->second.Build; }
		return it->second.Jobs;
	}

	Ctx BuildCtx(const std::vector<FightPtr>& aFights, int aIndex, bool aAllRounds, const std::string& aLeft, const std::string& aRight)
	{
		Ctx c;
		State& s = S();
		c.Fights = &aFights;
		c.Index = aIndex;
		c.F = aFights[aIndex].get();
		c.OneRound = !aAllRounds;
		c.Scope = aAllRounds ? aFights : std::vector<FightPtr>{aFights[aIndex]};
		if (c.F->Pov < 0) { return c; }
		c.MeRaw = &c.F->Players[c.F->Pov];
		for (const Player& p : c.F->Players) { if (!aLeft.empty() && p.Account == aLeft) { c.MeRaw = &p; break; } }
		c.You = Sum(c.Scope, c.MeRaw->Account, c.MeRaw->Spec, c.Names);
		// Role from all your rounds on this spec: one quiet round (or one without Healing Stats) doesn't change it
		std::map<int32_t, std::string> ignore;
		const Player tonight = Sum(aFights, c.MeRaw->Account, c.MeRaw->Spec, ignore);
		for (auto& [sk, r] : tonight.Skills) { if (r.Casts > 0 || r.Hits > 0) { c.UsedTonight.insert(sk); } }
		c.MyRole = RoleOf(tonight);
		c.Jobs = JobsFor(aFights, c.MeRaw->Account, c.MeRaw->Spec, &c.Build);
		// Your first job sets the role (the user's job lists); healing needs Healing Stats data to measure
		if (!c.Jobs.empty())
		{
			const std::string& first = c.Jobs[0];
			if (first == "Healing /s" && tonight.HealKnown) { c.MyRole = R_Heal; }
			else if (first == "Stability to subgroup") { c.MyRole = R_Stab; }
			else if (first == "Strips /min") { c.MyRole = R_Strip; }
			else if (first == "Damage to players /s") { c.MyRole = R_Damage; }
		}
		c.RoleMetric = c.MyRole == R_Heal ? kMetricHeal : c.MyRole == R_Stab ? kMetricGroupBoon + Analysis::kStability : c.MyRole == R_Strip ? kMetricStrips : kMetricDamage;

		// Peers: the same spec on the same build (the same job list); nobody else on it -> your role on any spec
		std::map<std::string, bool> seen;
		for (const FightPtr& fp : c.Scope)
		{
			for (const Player& p : fp->Players)
			{
				if (p.Spec != c.MeRaw->Spec || p.Account == c.MeRaw->Account || seen[p.Account]) { continue; }
				seen[p.Account] = true;
				if (JobsFor(aFights, p.Account, p.Spec) != c.Jobs) { continue; }
				c.Peers.push_back(Sum(c.Scope, p.Account, p.Spec, c.Names));
			}
		}
		// Nobody else on your spec this round: your own best round tonight on it (30 s+), so skill fixes work (the
		// user's choice, 2026-09-25); tonight as a whole, or with no other round of yours, your role on other specs
		if (c.Peers.empty() && c.OneRound)
		{
			const Metric& rm = Metrics()[c.RoleMetric];
			auto value = [&](const Player& p) { return c.MyRole == R_Stab ? (p.StabEligible ? double(p.StabCovered) / p.StabEligible : 0.0) : Rate(rm, p, rm.Total(p)); };
			int best = -1;
			double bestValue = 0;
			for (int i = 0; i < static_cast<int>(aFights.size()); i++)
			{
				if (i == aIndex || aFights[i]->DurationMs < 30000) { continue; }
				for (const Player& p : aFights[i]->Players)
				{
					if (p.Account != c.MeRaw->Account || p.Spec != c.MeRaw->Spec || !Known(rm, p)) { continue; }
					if (best < 0 || value(p) > bestValue) { best = i; bestValue = value(p); }
				}
			}
			if (best >= 0)
			{
				Player you = Sum({aFights[best]}, c.MeRaw->Account, c.MeRaw->Spec, c.Names);
				you.Name = "Round " + std::to_string(best + 1) + " (you)";
				you.Account += "#round"; // not you in this round: never matched as you
				you.Pov = false;
				c.Peers.push_back(you);
				c.BestOwnRound = best;
			}
		}
		if (c.Peers.empty())
		{
			c.SameSpec = false;
			for (const FightPtr& fp : c.Scope)
			{
				for (const Player& p : fp->Players)
				{
					std::string key = p.Account + "|" + p.Spec;
					if (p.Account == c.MeRaw->Account || seen[key] || RoleOf(p) != c.MyRole) { continue; }
					seen[key] = true;
					c.Peers.push_back(Sum(c.Scope, p.Account, p.Spec, c.Names));
				}
			}
		}
		const Metric& m = Metrics()[c.RoleMetric];
		std::sort(c.Peers.begin(), c.Peers.end(), [&](const Player& a, const Player& b)
		{
			if (Known(m, a) != Known(m, b)) { return Known(m, a); }
			return Rate(m, a, m.Total(a)) > Rate(m, b, m.Total(b));
		});
		// A picked player on another spec joins the list (only the Compare tab picks one)
		if (!aRight.empty() && aRight != c.MeRaw->Account && std::none_of(c.Peers.begin(), c.Peers.end(), [&](const Player& p) { return p.Account == aRight; }))
		{
			for (const Player& p : c.F->Players) { if (p.Account == aRight) { c.Peers.push_back(Sum(c.Scope, p.Account, p.Spec, c.Names)); break; } }
		}
		const std::string& want = aRight.empty() ? s.VsAccount : aRight;
		for (const Player& p : c.Peers) { if (p.Account == want) { c.Vs = &p; } }
		if (!c.Vs && !c.Peers.empty()) { c.Vs = &c.Peers[0]; }
		if (c.Vs)
		{
			for (const Player& p : c.F->Players) { if (p.Account == c.Vs->Account && p.Spec == c.Vs->Spec) { c.VsRaw = &p; } }
		}
		return c;
	}

	std::pair<std::vector<FightPtr>, std::string> Lookup(const Ctx& c, const Player& p)
	{
		static const std::string kTag = "#round";
		if (c.BestOwnRound >= 0 && p.Account.size() > kTag.size() && p.Account.compare(p.Account.size() - kTag.size(), kTag.size(), kTag) == 0)
		{
			return {{(*c.Fights)[c.BestOwnRound]}, p.Account.substr(0, p.Account.size() - kTag.size())};
		}
		return {c.Scope, p.Account};
	}

	// Building a context sums every player over the rounds in scope (and over tonight for the role): several ms
	// with a full evening loaded. The window draws every frame, so keep the last few and rebuild only when the
	// rounds or the choices behind them change. Each entry holds its own copy of the round list, which
	// Ctx::Fights points into.
	const Ctx& CachedCtx(const std::vector<FightPtr>& aFights, int aIndex, bool aAllRounds, const std::string& aLeft, const std::string& aRight)
	{
		struct Entry
		{
			std::vector<FightPtr> Fights;
			int Index = 0;
			bool AllRounds = false;
			std::string Left, Right, Vs;
			Ctx C;
			uint64_t Used = 0;
		};
		static std::vector<std::unique_ptr<Entry>> cache;
		static uint64_t clock = 0;
		clock++;
		const std::string& vs = S().VsAccount;
		for (auto& e : cache)
		{
			if (e->Index == aIndex && e->AllRounds == aAllRounds && e->Left == aLeft && e->Right == aRight && e->Vs == vs && e->Fights == aFights)
			{
				e->Used = clock;
				return e->C;
			}
		}
		constexpr size_t kEntries = 4; // the summary window, the main window, Compare with picked players, one spare
		if (cache.size() >= kEntries)
		{
			auto oldest = std::min_element(cache.begin(), cache.end(), [](const auto& a, const auto& b) { return a->Used < b->Used; });
			cache.erase(oldest);
		}
		auto e = std::make_unique<Entry>();
		e->Fights = aFights; e->Index = aIndex; e->AllRounds = aAllRounds; e->Left = aLeft; e->Right = aRight; e->Vs = vs; e->Used = clock;
		e->C = BuildCtx(e->Fights, aIndex, aAllRounds, aLeft, aRight);
		cache.push_back(std::move(e));
		return cache.back()->C;
	}

	// ---- why a skill gave less ----------------------------------------------------------------------------------------

	Why Explain(const Player& you, const Player& vs, const std::string& aVsName, int aMetric, int32_t aSkill, bool aOneRound, const std::string& aYouName)
	{
		// "You got ..." or "Ellyx got ..." when neither player is you
		const bool self = aYouName.empty();
		const std::string subj = self ? "You" : aYouName, obj = self ? "you" : aYouName;
		const std::string poss = self ? "your" : aYouName + "'s", possCap = self ? "Your" : aYouName + "'s";
		const std::string mine = self ? "yours" : aYouName + "'s", theirs = self ? "theirs" : aVsName + "'s";
		const Metric& m = Metrics()[aMetric];
		const char* unit = RateUnit(m, you);
		const SkillRow* ry = Row(you, aSkill);
		const SkillRow* rt = Row(vs, aSkill);
		double vy = ry ? m.PerSkill(*ry) : 0, vt = rt ? m.PerSkill(*rt) : 0;
		double rateY = Rate(m, you, vy), rateT = Rate(m, vs, vt);
		int cy = ry ? ry->Casts : 0, ct = rt ? rt->Casts : 0;
		Why w;
		w.Gap = rateT - rateY;
		auto output = [&]
		{
			w.You = rateY; w.Them = rateT; w.Unit = m.What == K_Boon ? Lower(m.Name) : WithUnit(Lower(m.Name), unit);
			w.YouText = Num(rateY); w.ThemText = Num(rateT);
		};
		auto counts = [&](double aYou, double aThem, const std::string& aUnit)
		{
			w.You = aYou; w.Them = aThem; w.Unit = aUnit; w.YouText = Num(aYou); w.ThemText = Num(aThem);
		};
		const std::string cost = WithUnit(Num(w.Gap), unit);
		if (w.Gap <= 0.0)
		{
			w.Word = rateY > 1.05 * rateT ? "more than " + aVsName : "about the same";
			w.Sentence = subj + " got " + WithUnit(Num(rateY), unit) + " from it, " + aVsName + " " + Num(rateT) + ".";
			output();
			return w;
		}
		if (cy == 0 && ct == 0)
		{
			// Nothing is cast: a boon or condition ticks, a trait, relic or sigil hits. Compare how often it landed.
			bool ticks = std::find(Analysis::kBoonIds.begin(), Analysis::kBoonIds.end(), uint32_t(aSkill)) != Analysis::kBoonIds.end()
				|| aSkill == 736 || aSkill == 737 || aSkill == 723 || aSkill == 861 || aSkill == 19426; // bleeding, burning, poison, confusion, torment
			const std::string what = ticks ? "ticks" : "hits";
			auto perMin = [](int aHits, const Player& p) { return p.ActiveMs > 0 ? aHits * 60000.0 / double(p.ActiveMs) : 0.0; };
			double hy = perMin(ry ? ry->Hits : 0, you), ht = perMin(rt ? rt->Hits : 0, vs);
			w.NoCasts = true;
			w.Word = (hy < 0.05 ? "no " : hy < 0.9 * ht ? "fewer " : "smaller ") + what;
			w.Sentence = "Nothing is cast: it comes from " + std::string(ticks ? "a boon or condition" : "a trait, relic or sigil") +
				". It landed " + Num(hy) + " times a minute for " + obj + ", " + Num(ht) + " for " + aVsName + ".";
			output();
			return w;
		}
		if (cy == 0)
		{
			w.Word = "not used";
			w.Sentence = subj + " didn't use it; it gave " + aVsName + " " + WithUnit(Num(rateT), unit) + ".";
			counts(0, ct, "casts");
			return w;
		}
		int cutY = ry->Interrupted + ry->Cancelled, cutT = rt ? rt->Interrupted + rt->Cancelled : 0;
		double shareCutY = double(cutY) / cy, shareCutT = ct ? double(cutT) / ct : 0.0;
		if (shareCutY >= 0.25 && shareCutY >= shareCutT + 0.15)
		{
			w.Word = "cut short";
			w.Sentence = std::to_string(cutY) + " of " + poss + " " + std::to_string(cy) + " casts were cut short, " +
				std::to_string(ry->Interrupted) + " of them by CC, a down or death.";
			counts(cutY, cutT, "casts cut short");
			return w;
		}
		// one boon skill's timing is no reason on its own: a build gives that boon from many skills, and what counts is
		// the boon on the ally when it's needed (the user, 2026-10-01: Power Break "cast late" on a Troubadour, 0 of 1
		// against 1 of 3). The boon's own measures say that (CC covered, stability that blocked CC, left over).
		int k = TimingWindow(aMetric);
		double inY = double(ry->Timing[k]) / cy, inT = ct ? double(rt->Timing[k]) / ct : 0.0;
		if (m.What != K_Boon && ct >= 3 && inT - inY >= 0.25)
		{
			w.Word = k == Analysis::T_AnsweringTheirs ? "off-beat" : k == Analysis::T_AheadOfTheirs ? "cast late" : "off the spike";
			w.Sentence = "Same skill, different moment: " + aVsName + " cast " + std::to_string(rt->Timing[k]) + " of " +
				std::to_string(ct) + " " + WindowLabel(k) + ", " + obj + " " + std::to_string(ry->Timing[k]) + " of " + std::to_string(cy) + ".";
			counts(ry->Timing[k], rt->Timing[k], std::string("casts ") + WindowLabel(k));
			return w;
		}
		double cpmY = PerS(you, cy) * 60, cpmT = PerS(vs, ct) * 60;
		double perY = vy / cy, perT = ct ? vt / ct : 0.0;
		if (cpmT > 1.15 * cpmY)
		{
			w.Word = "fewer casts";
			w.Sentence = aOneRound ? std::to_string(ct - cy) + " fewer casts cost " + obj + " " + cost + "."
				: "Fewer casts, " + Num(cpmY) + " against " + Num(cpmT) + " a minute, cost " + obj + " " + cost + ".";
			if (perY > 1.05 * perT) { w.Sentence += " Each of " + mine + " did more than " + theirs + "."; }
			if (aOneRound) { counts(cy, ct, "casts"); } else { counts(cpmY, cpmT, "casts /min"); }
			return w;
		}
		if (perT > 1.15 * perY)
		{
			w.Word = "less per cast";
			w.Sentence = possCap + " casts did less each: " + Num(perY) + " against " + Num(perT) + " per cast.";
			counts(perY, perT, "per cast");
			return w;
		}
		w.Word = "a bit less";
		w.Sentence = "Close: a few fewer casts and a little less per cast cost " + obj + " " + cost + ".";
		output();
		return w;
	}

	// ---- drawing ----------------------------------------------------------------------------------------------------------

	void SpecIcon(const Player& p)
	{
		float size = ImGui::GetTextLineHeight();
		if (void* icon = Icons::Get(p.ProfId, p.EliteId)) { ImGui::Image(icon, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ProfessionColor(p.ProfId)); }
		else { ImGui::Dummy(ImVec2(size, size)); }
		if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", p.Spec.c_str()); }
		ImGui::SameLine(0, 4);
	}

	namespace
	{
#include "SkillText.inc"
	}

	const char* SkillDescription(int32_t aSkill, const std::string& aName)
	{
		static const auto tables = []
		{
			std::pair<std::map<int32_t, const char*>, std::map<std::string, const char*>> t;
			for (const SkillText& s : kSkillTexts) { t.first[s.Skill] = s.Text; t.second.emplace(s.Name, s.Text); }
			return t;
		}();
		if (auto it = tables.first.find(aSkill); it != tables.first.end()) { return it->second; }
		if (auto it = tables.second.find(aName); it != tables.second.end()) { return it->second; }
		return "";
	}

	const char* BoonDescription(int aBoon)
	{
		static const char* kTexts[Analysis::kBoons] = {"more power and condition damage per stack", "more critical chance", "faster attacks and casts",
			"skills recharge faster", "a third less strike damage taken", "heals every second", "endurance comes back faster",
			"blocks the next attack", "each stack blocks one crowd control", "moves faster", "conditions do nothing but their damage",
			"a third less condition damage taken"};
		return aBoon >= 0 && aBoon < Analysis::kBoons ? kTexts[aBoon] : "";
	}

	void SkillIcon(int32_t aSkill, const std::string& aName)
	{
		float size = ImGui::GetTextLineHeight();
		if (void* icon = SkillIcons::Get(aSkill, aName)) { ImGui::Image(icon, ImVec2(size, size)); }
		else { ImGui::Dummy(ImVec2(size, size)); }
		ImGui::SameLine(0, 4);
	}

	void SpecIconAt(ImDrawList* dl, ImVec2 aPos, float aSize, const Player& p)
	{
		if (void* icon = Icons::Get(p.ProfId, p.EliteId))
		{
			dl->AddImage(icon, aPos, ImVec2(aPos.x + aSize, aPos.y + aSize), ImVec2(0, 0), ImVec2(1, 1), ImGui::ColorConvertFloat4ToU32(ProfessionColor(p.ProfId)));
			return;
		}
		// no icon (the render harness): the spec's first letter in the profession's colour
		char letter[2] = {p.Spec.empty() ? '?' : p.Spec[0], 0};
		dl->AddRect(aPos, ImVec2(aPos.x + aSize, aPos.y + aSize), ImGui::ColorConvertFloat4ToU32(ProfessionColor(p.ProfId)));
		dl->AddText(ImVec2(aPos.x + (aSize - ImGui::CalcTextSize(letter).x) * 0.5f, aPos.y), ImGui::ColorConvertFloat4ToU32(ProfessionColor(p.ProfId)), letter);
	}

	void IconAt(ImDrawList* dl, ImVec2 aPos, float aSize, int32_t aSkill, const std::string& aName, ImU32 aFrame)
	{
		ImVec2 b(aPos.x + aSize, aPos.y + aSize);
		if (void* icon = SkillIcons::Get(aSkill, aName)) { dl->AddImage(icon, aPos, b); }
		else
		{
			dl->AddRectFilled(aPos, b, IM_COL32(0x3b, 0x42, 0x50, 255));
			char letter[2] = {aName.empty() ? '?' : aName[0] == '"' && aName.size() > 1 ? aName[1] : aName[0], 0};
			ImVec2 t = ImGui::CalcTextSize(letter);
			float scale = std::min(1.0f, aSize / (ImGui::GetTextLineHeight() + 2));
			dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, ImVec2(aPos.x + (aSize - t.x * scale) * 0.5f, aPos.y + (aSize - t.y * scale) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_Text), letter);
		}
		if (aFrame) { dl->AddRect(ImVec2(aPos.x - 1, aPos.y - 1), ImVec2(b.x + 1, b.y + 1), aFrame, 0, 0, 1.5f); }
	}

	// Pseudo skill ids for the effect icons (SkillIcons finds them by name): CC types and boons
	void CcIconAt(ImDrawList* dl, ImVec2 aPos, float aSize, Analysis::CcKind aKind)
	{
		IconAt(dl, aPos, aSize, -100 - static_cast<int>(aKind), Analysis::kCcIcons[aKind], kEnemy);
	}

	void BoonIconAt(ImDrawList* dl, ImVec2 aPos, float aSize, int aBoon, bool aStruck)
	{
		// taken away (stripped or corrupted): a red outline along the icon's own shape, a square with a pointed roof
		// (measured on the game's 32 px boon icons: apex at the top middle, eaves at 6.5 of 32, walls at 2 and 31)
		IconAt(dl, aPos, aSize, -200 - aBoon, Analysis::kBoonNames[aBoon]);
		if (aStruck)
		{
			float k = aSize / 32.0f;
			ImVec2 pts[] = {ImVec2(aPos.x + 16 * k, aPos.y - 1), ImVec2(aPos.x + 32 * k, aPos.y + 6.5f * k), ImVec2(aPos.x + 32 * k, aPos.y + aSize + 1),
				ImVec2(aPos.x + 1 * k - 1, aPos.y + aSize + 1), ImVec2(aPos.x + 1 * k - 1, aPos.y + 6.5f * k)};
			dl->AddPolyline(pts, 5, kEnemy, true, 1.5f);
		}
	}

	// Shapes the font lacks (it has Latin-1 only): 0 square, 1 circle, 2 diamond, 3 triangle up, 4 triangle down
	void Mark(ImDrawList* dl, ImVec2 c, float r, int aShape, ImU32 aColor)
	{
		switch (aShape)
		{
		case 0: dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), aColor); break;
		case 1: dl->AddCircleFilled(c, r * 0.85f, aColor, 12); break;
		case 2: dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), aColor); break;
		case 3: dl->AddTriangleFilled(ImVec2(c.x - r, c.y + r * 0.8f), ImVec2(c.x + r, c.y + r * 0.8f), ImVec2(c.x, c.y - r * 0.9f), aColor); break;
		default: dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.8f), ImVec2(c.x + r, c.y - r * 0.8f), ImVec2(c.x, c.y + r * 0.9f), aColor); break;
		}
	}

	// A shape and a label on one line, for legends
	void ShapeKey(int aShape, ImU32 aColor, const char* aLabel)
	{
		float h = ImGui::GetTextLineHeight();
		ImVec2 p = ImGui::GetCursorScreenPos();
		Mark(ImGui::GetWindowDrawList(), ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.35f, aShape, aColor);
		ImGui::Dummy(ImVec2(h * 0.8f, h));
		ImGui::SameLine(0, 4);
		ImGui::TextColored(kMuted, "%s", aLabel);
		ImGui::SameLine(0, 12);
	}

	bool IsDamagePlayer(const std::vector<FightPtr>& aFights, const Player& p)
	{
		std::vector<std::string> jobs = JobsFor(aFights, p.Account, p.Spec);
		if (!jobs.empty()) { return jobs[0] == "Damage to players /s" || jobs[0] == "Strips /min"; } // strippers (Reapers) spike too
		return RoleOf(p) == R_Damage;
	}

	std::vector<Down> Downs(const Fight& f)
	{
		std::vector<Down> out;
		for (const Player& p : f.Players)
		{
			for (size_t i = 0; i < p.DownSpans.size(); i++)
			{
				const auto& sp = p.DownSpans[i];
				if (sp.Dead) { continue; }
				bool died = i + 1 < p.DownSpans.size() && p.DownSpans[i + 1].Dead && p.DownSpans[i + 1].From == sp.To;
				out.push_back({&p, sp, died});
			}
		}
		std::sort(out.begin(), out.end(), [](const Down& a, const Down& b) { return a.S.From < b.S.From; });
		return out;
	}

	std::string NoYou(const Fight& f)
	{
		return f.PovAbsent ? "You took no part in this round: no hit dealt or taken, never in combat (dead, at spawn or elsewhere)."
			: "This log's recorder isn't in the squad.";
	}

	std::string RevivedBy(const Fight& f, const Down& d, int32_t* aSkill)
	{
		*aSkill = 0;
		if (d.Died) { return ""; }
		const Player& p = *d.P;
		const int who = static_cast<int>(&p - f.Players.data());
		auto name = [&](int32_t aId) { auto it = f.SkillNames.find(aId); return it == f.SkillNames.end() ? std::to_string(aId) : it->second; };
		// an Illusion of Life on them as they got up
		for (const auto& il : p.IllusionOfLife)
		{
			if (std::abs(il.From - d.S.To) <= 300 && il.By >= 0) { *aSkill = 10244; return f.Players[il.By].Name + ", Illusion of Life"; }
		}
		// plain revives running at the get-up (several players can revive one ally together)
		std::string plain;
		for (const Player& q : f.Players)
		{
			for (const auto& v : q.Reviving)
			{
				if (v.Target == who && v.From <= d.S.To && v.To >= d.S.To - 300) { plain += (plain.empty() ? "" : ", ") + q.Name; break; }
			}
		}
		if (!plain.empty()) { *aSkill = 1066; return plain; }
		// healing on them while downed as they got up, from the healer's Healing Stats: the revive skill's own pulse
		// (Nature's Renewal from a Spirit of Nature, a Battle Standard, Signet of Mercy); the biggest one
		const Player::DownHeal* pulse = nullptr;
		for (const auto& h : p.DownHealsIn)
		{
			if (h.By >= 0 && h.By != who && std::abs(h.Ms - d.S.To) <= 400 && h.Amount >= 3000 && (!pulse || h.Amount > pulse->Amount)) { pulse = &h; }
		}
		if (pulse)
		{
			*aSkill = pulse->Skill == 12601 ? 12569 : pulse->Skill;
			return f.Players[pulse->By].Name + ", " + (pulse->Skill == 12601 ? std::string("Spirit of Nature") : name(pulse->Skill));
		}
		// a revive skill completed in the 3 s before, by an ally who had them in reach (ReviveUse counted them down near);
		// a Spirit of Nature also revives allies downed up to 1.5 s after it
		for (const Player& q : f.Players)
		{
			if (&q == &p) { continue; }
			for (const auto& u : q.ReviveUses)
			{
				const int32_t lag = u.Skill == 12569 ? 1500 : 0;
				if (u.Done && u.DownNear > 0 && u.Skill != 10244 && u.Ms + lag >= d.S.From && u.Ms <= d.S.To && d.S.To <= u.Ms + 3000) { *aSkill = u.Skill; return q.Name + ", " + name(u.Skill); }
			}
		}
		return "";
	}

	const Player::Point* NearestPos(const Player& p, int32_t aMs)
	{
		auto it = std::lower_bound(p.Pos.begin(), p.Pos.end(), aMs, [](const Player::Point& a, int32_t ms) { return a.Ms < ms; });
		if (it == p.Pos.end()) { return p.Pos.empty() ? nullptr : &p.Pos.back(); }
		if (it != p.Pos.begin() && aMs - (it - 1)->Ms < it->Ms - aMs) { --it; }
		return std::abs(it->Ms - aMs) <= 2000 ? &*it : nullptr;
	}

	bool Rallied(const Fight& f, const Down& d)
	{
		if (d.Died) { return false; }
		for (const auto& e : f.Enemies)
		{
			for (const auto& s : e.DownSpans) { if (s.Dead && std::abs(s.From - d.S.To) <= 100) { return true; } }
		}
		return false;
	}

	std::string GotUpHow(const Fight& f, const Down& d)
	{
		if (d.Died) { return ""; }
		int32_t skill = 0;
		std::string by = RevivedBy(f, d, &skill);
		if (!by.empty()) { return "revived by " + by; }
		if (Rallied(f, d)) { return "rallied: an enemy died at that moment"; }
		return "how is unknown (no revive, revive skill or enemy death seen)";
	}

	namespace { uint64_t s_DataVersion = 0; }
	uint64_t DataVersion() { return s_DataVersion; }
	void SetDataVersion(uint64_t aVersion) { s_DataVersion = aVersion; }

	bool HadBoonAt(const Player& p, int aBoon, int32_t aMs)
	{
		for (auto& [a, b] : p.BoonOn[aBoon]) { if (a <= aMs && b >= aMs) { return true; } }
		return false;
	}

	namespace { std::unordered_map<ImGuiID, uint32_t> s_NumCols; } // table -> its columns that held numbers (a bit each)

	// A number column's heading sits right, over its numbers (the user, 2026-10-01: headings at the left over numbers
	// at the right looked disconnected). Which columns hold numbers is known from NumCell, a frame late.
	void Headers(const std::vector<std::pair<const char*, const char*>>& aColumns)
	{
		ImGuiTable* table = ImGui::GetCurrentContext()->CurrentTable;
		uint32_t nums = table ? s_NumCols[table->ID] : 0;
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < static_cast<int>(aColumns.size()); c++)
		{
			ImGui::TableSetColumnIndex(c);
			if (c < 32 && (nums >> c & 1u))
			{
				ImGui::PushID(c);
				ImGui::TableHeader("##num");
				ImGui::PopID();
				const char* label = aColumns[c].first;
				ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				float w = ImGui::CalcTextSize(label, nullptr, true).x;
				ImGui::GetWindowDrawList()->AddText(ImVec2(b.x - table->CellPaddingX - w, a.y + ImGui::GetStyle().CellPadding.y), ImGui::GetColorU32(ImGuiCol_Text), label,
					ImGui::FindRenderedTextEnd(label));
			}
			else { ImGui::TableHeader(aColumns[c].first); }
			if (aColumns[c].second && ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", aColumns[c].second); }
		}
	}

	void Cell(const std::string& aText, const ImVec4* aColor)
	{
		ImGui::TableNextColumn();
		if (aColor) { ImGui::TextColored(*aColor, "%s", aText.c_str()); }
		else { ImGui::TextUnformatted(aText.c_str()); }
	}

	// A number, right-aligned in its column so units sit under units
	void NumCell(const std::string& aText, const ImVec4* aColor)
	{
		ImGui::TableNextColumn();
		if (ImGuiTable* table = ImGui::GetCurrentContext()->CurrentTable; table && table->CurrentColumn >= 0 && table->CurrentColumn < 32) { s_NumCols[table->ID] |= 1u << table->CurrentColumn; }
		float width = ImGui::CalcTextSize(aText.c_str()).x;
		float avail = ImGui::GetContentRegionAvail().x;
		if (avail > width) { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width); }
		if (aColor) { ImGui::TextColored(*aColor, "%s", aText.c_str()); }
		else { ImGui::TextUnformatted(aText.c_str()); }
	}

	// A value with a square-ended bar behind it, the bar's length relative to aMax
	void BarCell(double aValue, double aMax, ImU32 aColor, const std::string& aText, bool aOutline)
	{
		ImGui::TableNextColumn();
		ImVec2 pos = ImGui::GetCursorScreenPos();
		float width = ImGui::GetContentRegionAvail().x;
		float h = ImGui::GetTextLineHeight();
		if (aMax > 0 && aValue > 0)
		{
			float w = static_cast<float>(width * std::min(1.0, aValue / aMax));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(ImVec2(pos.x, pos.y + 1), ImVec2(pos.x + w, pos.y + h - 1), aColor);
			if (aOutline) { dl->AddRect(ImVec2(pos.x, pos.y + 1), ImVec2(pos.x + w, pos.y + h - 1), ImGui::GetColorU32(ImGuiCol_Text)); }
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 3, pos.y));
		ImGui::TextUnformatted(aText.c_str());
	}

	void Rect(ImDrawList* aList, ImVec2 aPos, float aWidth, float aHeight, ImU32 aColor)
	{
		if (aWidth >= 0.5f && aHeight > 0) { aList->AddRectFilled(aPos, ImVec2(aPos.x + aWidth, aPos.y + aHeight), aColor); }
	}

	void SmallText(ImDrawList* aList, ImVec2 aPos, ImU32 aColor, const std::string& aText)
	{
		aList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f, aPos, aColor, aText.c_str());
	}

	void Key(ImU32 aColor, const char* aLabel)
	{
		float h = ImGui::GetTextLineHeight();
		ImVec2 p = ImGui::GetCursorScreenPos();
		Rect(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + h * 0.3f), h * 0.9f, h * 0.4f, aColor);
		ImGui::Dummy(ImVec2(h * 0.9f, h));
		ImGui::SameLine(0, 4);
		ImGui::TextColored(kMuted, "%s", aLabel);
		ImGui::SameLine(0, 12);
	}

	void ScopeSwitch(const char* aId, bool& aTonight, const char* aTip)
	{
		ImGui::PushID(aId);
		float width = ImGui::CalcTextSize("This round").x + ImGui::CalcTextSize("Tonight").x + ImGui::GetStyle().FramePadding.x * 4 + 2;
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - width));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
		auto seg = [](const char* aLabel, bool aOn)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(aOn ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
			bool clicked = ImGui::SmallButton(aLabel);
			ImGui::PopStyleColor();
			return clicked;
		};
		if (seg("This round", !aTonight)) { aTonight = false; }
		ImGui::SameLine();
		if (seg("Tonight", aTonight)) { aTonight = true; }
		if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", aTip); }
		ImGui::PopStyleVar();
		ImGui::PopID();
	}

	void Answer(const std::string& aText)
	{
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(aText.c_str());
		ImGui::PopTextWrapPos();
	}

	void Lane(const Fight& f, const std::vector<int32_t>& aTimes, ImU32 aTick, float aWidth, float aHeight, int aWindow)
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
		auto x = [&](double aMs) { return p.x + static_cast<float>(std::clamp(aMs / span, 0.0, 1.0)) * aWidth; };
		dl->AddRectFilled(p, ImVec2(p.x + aWidth, p.y + aHeight), kLaneBg);
		for (int64_t t : f.OurSpikesMs) { dl->AddRectFilled(ImVec2(x(t - 2000.0), p.y), ImVec2(x(t + 2000.0), p.y + aHeight), kOurBand); }
		for (int64_t t : f.TheirSpikesMs)
		{
			if (aWindow == Analysis::T_AheadOfTheirs) { dl->AddRectFilled(ImVec2(x(t - 4000.0), p.y), ImVec2(x(double(t)), p.y + aHeight), kEnemyPre); }
			dl->AddRectFilled(ImVec2(x(double(t)), p.y), ImVec2(x(t + 3000.0), p.y + aHeight), kEnemyBand);
		}
		for (int32_t t : aTimes) { float tx = x(t); dl->AddRectFilled(ImVec2(tx, p.y + 1), ImVec2(tx + 2, p.y + aHeight - 1), aTick); }
		ImGui::Dummy(ImVec2(aWidth, aHeight));
	}

	void TimeAxis(const Fight& f, float aX, float aWidth)
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImU32 col = ImGui::GetColorU32(kMuted);
		for (int i = 0; i <= 4; i++)
		{
			std::string label = Duration(f.DurationMs * i / 4);
			float w = ImGui::CalcTextSize(label.c_str()).x * 0.85f;
			float lx = aX + aWidth * i / 4 - (i == 0 ? 0 : i == 4 ? w : w / 2);
			SmallText(dl, ImVec2(lx, p.y), col, label);
		}
		ImGui::Dummy(ImVec2(aWidth, ImGui::GetTextLineHeight()));
	}

	void SkillDetail(const Ctx& c, int aMetric, int32_t aSkill, bool aInCompare)
	{
		if (!c.Vs) { ImGui::TextColored(kMuted, "Nobody to compare with in this round."); return; }
		const Metric& m = Metrics()[aMetric];
		const std::string vsName = c.Vs->Name;
		Why w = Explain(c.You, *c.Vs, vsName, aMetric, aSkill, c.OneRound, c.LeftIsYou() ? "" : c.You.Name);
		Answer(w.Sentence);

		const SkillRow* ry = Row(c.You, aSkill);
		const SkillRow* rt = Row(*c.Vs, aSkill);
		double vy = ry ? m.PerSkill(*ry) : 0, vt = rt ? m.PerSkill(*rt) : 0;
		double rateY = Rate(m, c.You, vy), rateT = Rate(m, *c.Vs, vt);
		ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
		if (ImGui::BeginTable("detail", 6, flags))
		{
			std::string head = WithUnit(m.Name, RateUnit(m, c.You));
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 170);
			ImGui::TableSetupColumn(head.c_str(), ImGuiTableColumnFlags_WidthFixed, 200);
			for (int i = 0; i < 4; i++) { ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80); }
			Headers({{"", nullptr}, {head.c_str(), "From this skill"}, {"Casts", nullptr}, {"Casts /min", "Per minute alive"},
				{"Per cast", "Output of one cast"}, {"Cut short", "Stopped before it went off"}});
			auto line = [&](const Player& p, const SkillRow* r, double aRate, ImU32 aColor, const std::string& aName)
			{
				ImGui::TableNextRow();
				Cell(aName);
				BarCell(aRate, std::max(rateY, rateT), aColor, Num(aRate));
				int casts = r ? r->Casts : 0;
				NumCell(casts ? std::to_string(casts) : "-");
				NumCell(casts ? Num(PerS(p, casts) * 60) : "-");
				NumCell(casts && r ? Num(m.PerSkill(*r) / casts) : "-");
				NumCell(casts && r ? std::to_string(r->Interrupted + r->Cancelled) : "-");
			};
			line(c.You, ry, rateY, kYou, c.LeftName());
			line(*c.Vs, rt, rateT, kPeer, vsName);
			ImGui::EndTable();
		}

		// When you each cast it (one round: the lanes need the round's spikes)
		int k = TimingWindow(aMetric);
		const SkillRow* lY = c.MeRaw ? Row(*c.MeRaw, aSkill) : nullptr;
		const SkillRow* lT = c.VsRaw ? Row(*c.VsRaw, aSkill) : nullptr;
		if ((lY && lY->Casts) || (lT && lT->Casts))
		{
			ImGui::Spacing();
			ImGui::TextColored(kMuted, c.LeftIsYou() ? "When you each cast it" : "When each cast it");
			ImGui::SameLine(0, 20);
			Key(kOurBand, "our spike");
			Key(k == Analysis::T_AheadOfTheirs ? kEnemyPre : kEnemyBand, k == Analysis::T_AheadOfTheirs ? "4 s before an enemy spike" : "enemy spike and 3 s after");
			ImGui::NewLine();
			if (!c.OneRound) { ImGui::TextColored(kMuted, "Time lines show one round at a time: pick This round."); }
			else
			{
				const float labelW = 170, laneW = std::max(200.0f, ImGui::GetContentRegionAvail().x - labelW - 110);
				auto lane = [&](const char* aName, const SkillRow* r, ImU32 aTick)
				{
					ImGui::TextUnformatted(aName);
					ImGui::SameLine(labelW);
					static const std::vector<int32_t> kNone;
					Lane(*c.F, r ? r->CastMs : kNone, aTick, laneW, ImGui::GetTextLineHeight(), k);
					ImGui::SameLine();
					if (r && r->Casts)
					{
						ImGui::Text("%d of %d", r->Timing[k], r->Casts);
						if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%d of %d: of %d casts, %d came %s.", r->Timing[k], r->Casts, r->Casts, r->Timing[k], WindowLabel(k)); }
					}
					else { ImGui::TextColored(kMuted, "-"); }
				};
				lane(c.LeftName().c_str(), lY, kYou);
				lane(vsName.c_str(), lT, kPeerTick);
				TimeAxis(*c.F, ImGui::GetWindowPos().x - ImGui::GetScrollX() + labelW, laneW); // SameLine(x) is from the window's edge
			}
		}
		if (!aInCompare)
		{
			if (ImGui::SmallButton("Open in Compare")) { S().SwitchTo = T_Compare; S().Metric = aMetric; S().CompareOpen = aSkill; }
			ImGui::SameLine();
		}
		if (aSkill > 0 && ImGui::SmallButton("Mark it on the round"))
		{
			State& s = S();
			if (std::find(s.Picked.begin(), s.Picked.end(), aSkill) == s.Picked.end()) { s.Picked.push_back(aSkill); }
			s.SwitchTo = T_Round; s.RoundView = 0; s.SpikeOpen = -1;
		}
	}
}
