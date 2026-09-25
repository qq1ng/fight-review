#include "Analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "InstantCasts.h"

namespace Analysis
{
	const std::array<uint32_t, kBoons> kBoonIds = {740, 725, 1187, 30328, 717, 718, 726, 743, 1122, 719, 26980, 873};
	const std::array<const char*, kBoons> kBoonNames = {"Might", "Fury", "Quickness", "Alacrity", "Protection",
		"Regeneration", "Vigor", "Aegis", "Stability", "Swiftness", "Resistance", "Resolution"};
	const std::array<const char*, T_Count> kTimingNames = {"into ours", "ahead of theirs", "answering theirs"};
	const std::array<const char*, CC_Count> kCcVerbs = {"stunned", "dazed", "stunned or dazed", "knocked down", "knocked back", "pulled",
		"knocked back or pulled", "launched", "floated", "feared", "taunted", "staggered", "crowd controlled"};
	const std::array<const char*, CC_Count> kCcIcons = {"Stun", "Daze", "Stun", "Knockdown", "Knockback", "Pull", "Knockback", "Launch",
		"Launch", "Fear", "Taunt", "Knockdown", "Stun"};

	namespace
	{
		using namespace Evtc;

		constexpr uint8_t kHealDowned = 1, kHealSelfReported = 128; // Healing Stats flags in is_offcycle
		constexpr int32_t kWeaponSwap = -2;
		constexpr uint8_t kResultCrowdControl = 12; // cbtresult: target was crowd controlled; value = duration
		constexpr uint8_t kActivationCancel = 4;    // cbtanimation: stopped before the trigger point
		constexpr int64_t kCastSlack = 250;   // attribution: an animated cast covers its animation plus this
		constexpr int64_t kInstantSlack = 100; // and an instant cast this much either side
		constexpr int64_t kBoonDelay = 750;    // some skills grant their boons this long after the cast
		// Tuned with tools/spike_sweep.py (2026-09-24): trained on 56 rounds, checked on 110 others
		constexpr int64_t kSpikeRadiusS = 2;
		constexpr double kSpikeOverMedian = 1.4, kSpikeOfMax = 0.5;
		constexpr int64_t kIntoOurs = 2000, kAheadOfTheirs = 4000, kAnsweringTheirs = 3000;
		constexpr uint32_t kResurrect = 1066;     // the plain revive: animation dst = the ally
		constexpr uint8_t kStopCompleted = 22;    // n_animationstop: the cast went through
		constexpr uint32_t kDodge = 23275;        // the Dodge skill's animation (Elite Insights: ArcDPSDodge20220307)
		constexpr float kReviveReach = 1500;      // cast range plus radius of the revive skills, game units

		// Revive skills and their base cast time (notes/HANDOFF.md section 5, local): a stop at or past it is a completed use
		int32_t ReviveCastMs(uint32_t aSkill)
		{
			switch (aSkill)
			{
			case 9163: case 9243: case 5573: case 5760: case 5761: case 5762: case 5763: case 24407: case 24409:
			case 24410: case 24411: case 14419: return 2000;
			case 10244: return 1250;
			case 10611: case 12569: return 1500;
			default: return 0;
			}
		}

		const std::unordered_set<uint32_t> kConditions = {736, 737, 861, 723, 19426, 720, 722, 721, 791, 727, 26766,
			27705, 742, 738};

		bool IsDamageResult(uint8_t aResult)
		{
			switch (aResult)
			{
			case 0: case 1: case 2: case 5: case 8: case 9: case 10: case 13: case 14: case 15: case 16: case 17: case 18:
				return true;
			default:
				return false;
			}
		}

		int BoonIndex(uint32_t aSkill)
		{
			for (int i = 0; i < kBoons; i++) { if (kBoonIds[i] == aSkill) { return i; } }
			return -1;
		}

		std::vector<int64_t> Spikes(const std::vector<int64_t>& aSeries)
		{
			std::vector<int64_t> busy;
			for (int64_t v : aSeries) { if (v > 0) { busy.push_back(v); } }
			std::vector<int64_t> out;
			if (busy.empty()) { return out; }
			std::sort(busy.begin(), busy.end());
			double median = busy.size() % 2 ? busy[busy.size() / 2] : (busy[busy.size() / 2 - 1] + busy[busy.size() / 2]) / 2.0;
			double floor = std::max(kSpikeOverMedian * median, kSpikeOfMax * busy.back());
			int64_t last = -1000;
			for (int64_t i = 0; i < static_cast<int64_t>(aSeries.size()); i++)
			{
				int64_t v = aSeries[i];
				if (v < floor) { continue; }
				bool peak = true;
				for (int64_t j = std::max<int64_t>(0, i - kSpikeRadiusS); j <= std::min<int64_t>(aSeries.size() - 1, i + kSpikeRadiusS); j++)
				{
					if (aSeries[j] > v) { peak = false; break; }
				}
				if (peak && i - last > kSpikeRadiusS)
				{
					out.push_back(i * 1000 + 500);
					last = i;
				}
			}
			return out;
		}

		std::array<bool, T_Count> Classify(int64_t aT, const Fight& aFight)
		{
			std::array<bool, T_Count> c{};
			for (int64_t s : aFight.OurSpikesMs) { if (std::llabs(aT - s) <= kIntoOurs) { c[T_IntoOurs] = true; } }
			for (int64_t s : aFight.TheirSpikesMs)
			{
				if (s - aT > 0 && s - aT <= kAheadOfTheirs) { c[T_AheadOfTheirs] = true; }
				if (aT - s >= 0 && aT - s <= kAnsweringTheirs) { c[T_AnsweringTheirs] = true; }
			}
			return c;
		}

		struct Window { int64_t Start, End; int32_t Skill; bool Instant; };

		// The skill whose cast explains something at aT: the closest instant cast around it, else the latest
		// animated cast in progress, else 0 (traits, relics, sigils).
		// Skills whose cast could explain something at aT, most likely first: instant casts around it (closest
		// first), then animated casts in progress (latest first).
		std::vector<int32_t> Candidates(const std::vector<Window>& aWindows, int64_t aT)
		{
			std::vector<std::pair<int64_t, int32_t>> instant;
			std::vector<int32_t> animated;
			for (const Window& w : aWindows)
			{
				if (w.Start > aT) { break; }
				if (aT > w.End) { continue; }
				if (w.Instant) { instant.push_back({std::llabs(aT - (w.Start + kInstantSlack)), w.Skill}); }
				else { animated.push_back(w.Skill); }
			}
			std::stable_sort(instant.begin(), instant.end(), [](auto& a, auto& b) { return a.first < b.first; });
			std::vector<int32_t> out;
			for (auto& [gap, skill] : instant) { out.push_back(skill); }
			out.insert(out.end(), animated.rbegin(), animated.rend());
			return out;
		}

		int32_t Attribute(const std::vector<Window>& aWindows, int64_t aT)
		{
			auto c = Candidates(aWindows, aT);
			return c.empty() ? 0 : c[0];
		}

		struct Stack { uint64_t Source; int Boon; int64_t Since; int64_t Applied; bool Counting; int32_t Duration; };

		struct Hit { int64_t Time; uint64_t Target; int32_t Skill; };

		// One stability stack: who gave it to whom, when it was there, and when it was due to run out
		struct StabStack { uint64_t Source, Target; int64_t Applied, Removed, NominalEnd; };
		constexpr int64_t kCcWindowMs = 750;   // TopStats: a CC contact opens a 0.75 s window (stability's cooldown)
		constexpr int64_t kReadyMs = 3000;     // TopStats: "Ready" = 3+ s of stability left
		constexpr int64_t kConsumedMs = 50;    // a stack removed this close before a CC contact was consumed by it
		constexpr int64_t kSameMoment = 10; // ms: Elite Insights' server delay

#include "RemovalSkills.inc"

		// A cleanse or strip goes to a skill only if the GW2 API says that skill removes conditions (boons):
		// first one whose direct heal or hit landed on the same target at the same moment, then one being
		// cast. Otherwise 0: a trait, relic, sigil or combo did it (the log doesn't say which).
		int32_t AttributeRemoval(const std::vector<Hit>& aEvents, int64_t aT, uint64_t aTarget,
			const std::vector<Window>& aWindows, const std::unordered_set<int32_t>& aRemovers)
		{
			auto first = std::lower_bound(aEvents.begin(), aEvents.end(), aT - kSameMoment,
				[](const Hit& h, int64_t t) { return h.Time < t; });
			for (auto it = first; it != aEvents.end() && it->Time <= aT + kSameMoment; ++it)
			{
				if (it->Target == aTarget && aRemovers.count(it->Skill)) { return it->Skill; }
			}
			for (int32_t skill : Candidates(aWindows, aT)) { if (aRemovers.count(skill)) { return skill; } }
			return 0;
		}

#include "BoonSkills.inc"
#include "UseBuffs.inc"

		// Which boons a skill gives: the GW2 API's facts, plus what the logs show for this player (skill -> boon bits).
		// Boons of anything else go to 0 (a trait, relic, sigil, rune or combo).
		using Givers = std::unordered_map<int32_t, uint32_t>;
		uint64_t GiverKey(uint32_t aProf, int32_t aSkill) { return static_cast<uint64_t>(aProf) << 32 | static_cast<uint32_t>(aSkill); }

		// How long after a use ends a skill keeps giving each boon, in ms, beyond kBoonDelay (pulsing skills; learned)
		using Reach = std::unordered_map<int32_t, std::array<int32_t, kBoons>>;

		// Boons a skill doesn't give although the logs make it look so (it is used together with the real source, a
		// trait or another skill): corrections the user made, which learning never overrides. Bit mask in kBoonNames order.
		uint32_t NotGiven(int32_t aSkill)
		{
			switch (aSkill)
			{
			case 10214: case 10215: return 1u << kStability; // Power Return (the user, 2026-09-25: it gives no stability)
			default: return 0;
			}
		}

		int32_t AttributeBoon(const std::vector<Window>& aWindows, int64_t aT, int aBoon, const Givers& aLearned, const Reach& aReach)
		{
			static const std::unordered_map<int32_t, uint32_t> api(std::begin(kBoonSkills), std::end(kBoonSkills));
			auto gives = [&](int32_t aSkill)
			{
				auto it = api.find(aSkill);
				if (it != api.end() && (it->second >> aBoon & 1)) { return true; }
				auto lt = aLearned.find(aSkill);
				return lt != aLearned.end() && (lt->second >> aBoon & 1) != 0;
			};
			for (int32_t skill : Candidates(aWindows, aT)) { if (gives(skill)) { return skill; } }
			// Nothing at that moment: a use that ended shortly before (a boon granted a moment after the cast)
			for (int64_t back = 250; back <= kBoonDelay; back += 250)
			{
				for (int32_t skill : Candidates(aWindows, aT - back)) { if (gives(skill)) { return skill; } }
			}
			// Still nothing: the latest use of a pulsing skill whose pulses reach this far
			auto from = std::lower_bound(aWindows.begin(), aWindows.end(), aT - (kLagSeconds + 1) * 1000,
				[](const Window& w, int64_t t) { return w.Start < t; });
			int32_t best = 0;
			for (auto it = from; it != aWindows.end() && it->Start <= aT; ++it)
			{
				auto r = aReach.find(it->Skill);
				if (r != aReach.end() && aT - it->End <= r->second[aBoon] && gives(it->Skill)) { best = it->Skill; }
			}
			return best;
		}

		// Sources told apart by the duration they give: each has its own base duration, and a player's boon duration
		// scales them all alike. The user named these (2026-09-24, from the in-game boon sources and traits); on
		// 23 Sept one Firebrand gave stability at exactly 12, 10, 8, 6 and 4 s, the others at the same ratios.
		struct DurationSource { const char* Spec; int Boon; int32_t BaseMs; int32_t Skill; const char* Name; };
		const DurationSource kDurationSources[] = {
			{"Firebrand", kStability, 12000, 9153, "\"Stand Your Ground!\""},
			{"Firebrand", kStability, 10000, 41328, "Unhindered Delivery"},
			{"Firebrand", kStability, 8000, 42259, "Tome of Courage (Indomitable Courage)"},
			{"Firebrand", kStability, 6000, 40114, "Portent of Freedom"},
			{"Firebrand", kStability, 4000, 9253, "Hallowed Ground"},
		};
		constexpr double kDurationMatch = 0.03; // within 3% of a base after scaling

		// The player's scale for a boon (their boon duration): the factor that puts the most of their durations on a
		// base. 0 when the spec has no table or too few durations fit.
		double DurationScale(const std::string& aSpec, int aBoon, const std::vector<int32_t>& aDurations)
		{
			std::vector<double> bases;
			for (const DurationSource& d : kDurationSources) { if (aSpec == d.Spec && d.Boon == aBoon) { bases.push_back(d.BaseMs); } }
			if (bases.empty() || aDurations.size() < 5) { return 0; }
			double best = 0;
			size_t bestFit = 0;
			for (double k = 0.5; k <= 2.5; k += 0.002)
			{
				size_t fit = 0;
				for (int32_t d : aDurations)
				{
					for (double base : bases) { if (std::abs(d / k - base) <= kDurationMatch * base) { fit++; break; } }
				}
				if (fit > bestFit || (fit == bestFit && std::abs(k - 1) < std::abs(best - 1))) { bestFit = fit; best = k; }
			}
			return bestFit * 2 >= aDurations.size() ? best : 0;
		}

		// The source a duration points to, 0 if none
		int32_t SourceByDuration(const std::string& aSpec, int aBoon, int32_t aDuration, double aScale)
		{
			if (aScale <= 0 || aDuration <= 0) { return 0; }
			for (const DurationSource& d : kDurationSources)
			{
				if (aSpec == d.Spec && d.Boon == aBoon && std::abs(aDuration / aScale - d.BaseMs) <= kDurationMatch * d.BaseMs) { return d.Skill; }
			}
			return 0;
		}

		// Uses the log shows without a cast event. A profession mechanic (F1-F5, any spec of the class) puts a
		// buff of its own name on the player when switched on: tomes, shrouds, attunements, legendary stances,
		// Berserk, chants. A signet's passive buff comes off when the signet is used.
		enum UseBuff { U_None, U_Mode, U_Signet };
		UseBuff UseBuffKind(const std::string& aName)
		{
			static const std::unordered_set<std::string> modes(std::begin(kProfessionSkillNames), std::end(kProfessionSkillNames));
			static const std::unordered_set<std::string> signets(std::begin(kSignetNames), std::end(kSignetNames));
			return modes.count(aName) ? U_Mode : signets.count(aName) ? U_Signet : U_None;
		}
	}

	double Fight::SquadGeneration(const Player& aPlayer, int aBoon) const
	{
		if (SquadCount < 2 || DurationMs <= 0) { return 0.0; }
		double value = aPlayer.BoonSquadS[aBoon] * 1000.0 / (static_cast<double>(DurationMs) * (SquadCount - 1));
		return Intensity[aBoon] ? value : 100.0 * value;
	}

	double Fight::GroupGeneration(const Player& aPlayer, int aBoon) const
	{
		auto it = GroupSize.find(aPlayer.Subgroup);
		if (it == GroupSize.end() || it->second < 2 || DurationMs <= 0) { return 0.0; }
		double value = aPlayer.BoonGroupS[aBoon] * 1000.0 / (static_cast<double>(DurationMs) * (it->second - 1));
		return Intensity[aBoon] ? value : 100.0 * value;
	}

	// Whether a character name tells a player apart (Rezz Order's rule): none has a digit (Edge of the Mists shows
	// placeholders like "ag1458"), and WvW rank names ("Mithril Champion") repeat between players
	bool UsableCharacterName(const std::string& aName)
	{
		if (aName.empty()) { return false; }
		for (unsigned char ch : aName) { if (ch >= '0' && ch <= '9') { return false; } }
		static const char* kTiers[] = {"Bronze", "Silver", "Gold", "Platinum", "Mithril", "Diamond"};
		static const char* kRanks[] = {"Invader", "Assaulter", "Raider", "Recruit", "Scout", "Soldier", "Squire", "Footman", "Knight",
			"Major", "Colonel", "General", "Veteran", "Champion", "Legend", "Dominator", "Conqueror", "Vanquisher", "Warlord", "Marshal"};
		size_t space = aName.find(' ');
		if (space == std::string::npos || aName.find(' ', space + 1) != std::string::npos) { return true; }
		std::string first = aName.substr(0, space), second = aName.substr(space + 1);
		bool tier = std::any_of(std::begin(kTiers), std::end(kTiers), [&](const char* t) { return first == t; });
		bool rank = std::any_of(std::begin(kRanks), std::end(kRanks), [&](const char* r) { return second == r; });
		return !(tier && rank);
	}

	uint32_t LegendOf(const std::string& aBuffName)
	{
		static const std::pair<const char*, uint32_t> kLegends[] = {
			{"Legendary Centaur Stance", L_Centaur}, {"Legendary Demon Stance", L_Demon}, {"Legendary Dwarf Stance", L_Dwarf},
			{"Legendary Assassin Stance", L_Assassin}, {"Legendary Dragon Stance", L_Dragon}, {"Legendary Renegade Stance", L_Renegade},
			{"Legendary Alliance Stance", L_Alliance}, {"Legendary Entity Stance", L_Entity}};
		for (auto& [name, bit] : kLegends) { if (aBuffName == name) { return bit; } }
		return 0;
	}

	void AddEvidence(Evidence& aTo, const Evidence& aFrom, int aSign)
	{
		auto add = [aSign](auto& aMap, const auto& aKey, const BoonEvidence& ev)
		{
			BoonEvidence& to = aMap[aKey];
			to.Uses += aSign * ev.Uses;
			for (int b = 0; b < kBoons; b++)
			{
				to.Followed[b] += aSign * ev.Followed[b];
				to.Expected[b] += aSign * ev.Expected[b];
				for (int k = 0; k < kLagSeconds; k++) { to.After[b][k] += aSign * ev.After[b][k]; to.AfterExpected[b][k] += aSign * ev.AfterExpected[b][k]; }
			}
			if (to.Uses <= 0) { aMap.erase(aKey); }
		};
		for (auto& [key, ev] : aFrom.ByProfession) { add(aTo.ByProfession, key, ev); }
		for (auto& [key, ev] : aFrom.ByPlayer) { add(aTo.ByPlayer, key, ev); }
	}

	Fight Analyse(const std::filesystem::path& aPath, const Evidence* aOthers, bool aOthersHaveThis)
	{
		Log log = Load(aPath);
		Fight f;
		f.Path = aPath;
		f.Stamp = aPath.stem().string();

		// Pre-pass: window, point of view, stacking types, minion masters, presence
		int64_t start = INT64_MAX, end = INT64_MIN, strikeMin = INT64_MAX, strikeMax = INT64_MIN;
		uint64_t pov = 0;
		f.Intensity[0] = f.Intensity[kStability] = true; // might, stability, unless BUFFINFO says otherwise
		std::unordered_map<uint16_t, uint64_t> instToPlayer;
		std::unordered_set<uint64_t> friends, foes, squad, present;
		for (auto& [addr, a] : log.Agents)
		{
			if (!a.Player) { continue; }
			if (a.Account.empty()) { foes.insert(addr); } else { friends.insert(addr); }
			if (a.Subgroup > 0) { squad.insert(addr); }
		}
		for (const Event& e : log.Events)
		{
			switch (e.StateChange)
			{
			case SC_None:
				strikeMin = std::min<int64_t>(strikeMin, e.Time);
				strikeMax = std::max<int64_t>(strikeMax, e.Time);
				if (const Agent* a = log.Find(e.Src); a && a->Player) { instToPlayer.emplace(e.SrcInst, e.Src); }
				if (squad.count(e.Src)) { present.insert(e.Src); }
				if (squad.count(e.Dst)) { present.insert(e.Dst); }
				break;
			case SC_EnterCombat:
				if (squad.count(e.Src)) { present.insert(e.Src); }
				break;
			case SC_SqCombatStart: start = std::min<int64_t>(start, e.Time); break;
			case SC_SqCombatEnd: end = std::max<int64_t>(end, e.Time); break;
			case SC_PointOfView: pov = e.Src; break;
			case SC_MapId: f.MapId = static_cast<uint32_t>(e.Src); break;
			case SC_BuffInfo:
				if (int b = BoonIndex(e.Skill); b >= 0) { uint8_t type = e.Pad61 & 0xFF; f.Intensity[b] = type == 0 || type == 4; }
				break;
			default: break;
			}
		}
		if (strikeMin == INT64_MAX) { throw std::runtime_error("no combat in this log"); }
		if (start == INT64_MAX || end == INT64_MIN || end <= start) { start = strikeMin; end = strikeMax; }
		f.DurationMs = end - start;
		auto owner = [&](uint64_t aAddr, uint16_t aMasterInst) -> uint64_t
		{
			if (const Agent* a = log.Find(aAddr); a && a->Player) { return aAddr; }
			if (aMasterInst) { auto it = instToPlayer.find(aMasterInst); if (it != instToPlayer.end()) { return it->second; } }
			return 0;
		};

		// the recorder in the squad but in no hit and never in combat (dead, at spawn, elsewhere): not "not in the squad"
		f.PovAbsent = squad.count(pov) && !present.count(pov);
		std::unordered_map<uint64_t, int> index;
		for (uint64_t addr : present)
		{
			const Agent& a = log.Agents[addr];
			Player p;
			p.Addr = addr;
			p.Name = a.Name;
			p.Character = a.Name;
			p.Account = a.Account;
			p.Spec = SpecName(a);
			p.Profession = ProfessionName(a.Prof);
			p.ProfId = a.Prof;
			p.EliteId = a.Elite;
			p.Subgroup = a.Subgroup;
			p.Pov = addr == pov;
			index[addr] = static_cast<int>(f.Players.size());
			f.Players.push_back(std::move(p));
		}
		// Names: account names in Edge of the Mists, or when a character name can't be told apart (a placeholder with a
		// digit, a WvW rank, or the same name twice); the part before the dot, the whole account when two share it
		{
			std::map<std::string, int> seen;
			for (const Player& p : f.Players) { seen[p.Character]++; }
			bool accounts = f.MapId == 968;
			for (const Player& p : f.Players) { accounts |= !UsableCharacterName(p.Character) || seen[p.Character] > 1; }
			if (accounts)
			{
				f.AccountNames = true;
				auto shortName = [](const std::string& aAccount)
				{
					size_t dot = aAccount.rfind('.');
					return dot == std::string::npos ? aAccount : aAccount.substr(0, dot);
				};
				std::map<std::string, int> shortCount;
				for (const Player& p : f.Players) { shortCount[shortName(p.Account)]++; }
				for (Player& p : f.Players) { p.Name = shortCount[shortName(p.Account)] > 1 ? p.Account : shortName(p.Account); }
			}
		}
		std::sort(f.Players.begin(), f.Players.end(), [](const Player& x, const Player& y)
			{ return x.Subgroup != y.Subgroup ? x.Subgroup < y.Subgroup : x.Name < y.Name; });
		index.clear();
		for (int i = 0; i < static_cast<int>(f.Players.size()); i++)
		{
			index[f.Players[i].Addr] = i;
			if (f.Players[i].Pov) { f.Pov = i; }
		}
		f.SquadCount = static_cast<int>(f.Players.size());
		auto player = [&](uint64_t aAddr) -> Player* { auto it = index.find(aAddr); return it == index.end() ? nullptr : &f.Players[it->second]; };

		std::unordered_set<uint64_t> enemiesSeen;
		std::unordered_map<uint64_t, int> enemyIndex;
		auto enemy = [&](uint64_t aAddr) -> int
		{
			if (!foes.count(aAddr)) { return -1; }
			auto [it, added] = enemyIndex.emplace(aAddr, static_cast<int>(f.Enemies.size()));
			if (added) { f.Enemies.push_back({SpecName(log.Agents.at(aAddr)), {}}); }
			return it->second;
		};
		const size_t seconds = static_cast<size_t>(f.DurationMs / 1000 + 1);
		f.OutPerS.assign(seconds, 0);
		std::vector<int64_t> toPlayersPerS(seconds, 0); // our damage to enemy players: what our spikes are about
		f.InPerS.assign(seconds, 0);
		f.SupportPerS.assign(seconds, 0);
		std::unordered_map<uint64_t, std::vector<Window>> windows;
		std::unordered_map<uint64_t, std::vector<std::pair<int64_t, int32_t>>> casts; // (time from start, skill)
		// (time, target) of each strip / cleanse; (time, target, skill) of each heal and hit, for attribution
		std::unordered_map<uint64_t, std::vector<std::pair<int64_t, uint64_t>>> stripTimes, cleanseTimes;
		std::unordered_map<uint64_t, std::vector<Hit>> heals, hits;
		std::unordered_map<uint64_t, int64_t> deadSince, deadMs;
		std::vector<StabStack> stab;
		std::unordered_map<uint64_t, size_t> stabIndex;                   // stack key -> index into stab
		std::unordered_map<uint64_t, std::vector<int64_t>> ccContacts;    // recipient -> CC contact times
		std::unordered_map<uint64_t, std::vector<std::pair<int64_t, int64_t>>> cannotAct; // downed or dead
		std::unordered_map<uint64_t, int64_t> cannotActSince;
		std::unordered_map<uint64_t, int64_t> downOpen, deadOpen;          // for the downed / dead spans
		std::unordered_map<uint64_t, std::pair<int64_t, int>> revivingOpen; // plain revive in progress: start, ally
		auto rel = [&](int64_t aT) { return static_cast<int32_t>(std::clamp<int64_t>(aT - start, 0, end - start)); };
		for (Player& p : f.Players) { p.DamagePerS.assign(seconds, 0); p.HealPerS.assign(seconds, 0); }
		// Uses without a cast event (mode buffs, signets), and when each player applied each boon (evidence)
		std::unordered_map<uint32_t, UseBuff> useKinds;
		std::unordered_map<uint32_t, uint32_t> legendOf; // buff id -> Legend bit (0: not a legend)
		auto useKind = [&](uint32_t aId)
		{
			auto it = useKinds.find(aId);
			if (it == useKinds.end()) { it = useKinds.emplace(aId, UseBuffKind(log.SkillName(aId))).first; }
			return it->second;
		};
		std::unordered_map<uint64_t, int64_t> lastUse; // player ^ buff -> time, one use per switch
		struct SignetOff { uint64_t Player; uint32_t Buff; int64_t Time; };
		std::unordered_map<uint64_t, SignetOff> signetOff; // player ^ buff -> when its passive came off
		auto noteUse = [&](uint64_t aPlayer, uint32_t aId, int64_t aT)
		{
			uint64_t key = aPlayer * 1000003ULL ^ aId;
			auto it = lastUse.find(key);
			if (it != lastUse.end() && aT - it->second < 1000) { return; }
			lastUse[key] = aT;
			casts[aPlayer].push_back({aT - start, static_cast<int32_t>(aId)});
			windows[aPlayer].push_back({aT - kInstantSlack, aT + kInstantSlack, static_cast<int32_t>(aId), true});
		};
		std::unordered_map<uint64_t, std::array<std::vector<int64_t>, kBoons>> boonApplied; // source -> boon -> times
		std::unordered_map<uint64_t, std::array<std::vector<int32_t>, kBoons>> boonDurations; // source -> boon -> durations given to others
		std::unordered_map<uint64_t, int64_t> lastStabStrip; // holder -> when the enemy last removed all their stability
		uint64_t commander = 0;
		// Boons
		std::unordered_map<uint64_t, Stack> stacks; // key: target ^ (stack id << 1) collisions avoided below
		struct Credit { uint64_t Source, Target; int Boon; int64_t Applied, Ms, From; int32_t Duration; };
		std::vector<Credit> credits;
		// CC types and corruptions are settled after the events, once the effects applied with them and the positions
		// are known
		struct RawCc { Player* P; size_t Index; uint32_t Type; uint64_t By; int64_t T; };
		std::vector<RawCc> rawCc;
		std::unordered_map<uint64_t, std::vector<std::pair<int64_t, CcKind>>> controlApplied; // player -> Stun, Daze, Fear, Taunt effects
		std::unordered_map<uint32_t, int> controlOf;                                           // buff id -> CcKind, -1: not one
		std::unordered_map<uint64_t, std::vector<std::pair<int64_t, uint64_t>>> condApplied;   // player -> (time, source) of conditions
		struct RawStrip { Player* P; size_t Index; uint64_t By; int64_t T; };
		std::vector<RawStrip> rawStrips;
		std::unordered_map<int, int64_t> enemyDownOpen, enemyDeadOpen; // enemy index -> since
		std::unordered_map<uint32_t, bool> illusionIds;               // buff id -> is Illusion of Life
		std::unordered_map<uint64_t, int32_t> illusionLength;         // player -> the Illusion of Life's duration
		auto isIllusion = [&](uint32_t aId)
		{
			auto it = illusionIds.find(aId);
			if (it == illusionIds.end()) { it = illusionIds.emplace(aId, log.SkillName(aId) == "Illusion of Life").first; }
			return it->second;
		};
		auto stackKey = [](uint64_t aTarget, uint32_t aId) { return aTarget * 1000003ULL ^ aId; };
		std::unordered_map<uint64_t, uint64_t> stackTarget; // key -> target, to detect collisions
		auto stop = [&](uint64_t aKey, uint64_t aTarget, int64_t aT)
		{
			auto it = stacks.find(aKey);
			if (it == stacks.end() || !it->second.Counting) { return; }
			int64_t t0 = std::max(it->second.Since, start), t1 = std::min(aT, end);
			if (t1 > t0) { credits.push_back({it->second.Source, aTarget, it->second.Boon, it->second.Applied, t1 - t0, t0, it->second.Duration}); }
			it->second.Counting = false;
		};

		for (const Event& e : log.Events)
		{
			const int64_t t = static_cast<int64_t>(e.Time);
			switch (e.StateChange)
			{
			case SC_None:
			{
				if (e.Dst && friends.count(e.Dst))
				{
					if (Player* p = player(e.Dst))
					{
						if (e.Result == RESULT_ABSORB) { p->Invulns++; }
						else if (!e.Buff && e.Result == RESULT_EVADE)
						{
							p->Evades++;
							if (uint64_t o = owner(e.Src, e.SrcMaster); foes.count(o)) { p->EvadedIn.push_back({rel(t), static_cast<int32_t>(e.Skill), 0, enemy(o)}); }
						}
						else if (!e.Buff && e.Result == RESULT_BLOCK) { p->Blocks++; }
					}
				}
				if (e.Result == kResultCrowdControl && !e.Buff)
				{
					if (Player* p = player(e.Dst))
					{
						p->CcTaken++; p->CcTakenMs += std::max(0, e.Value); ccContacts[e.Dst].push_back(t); p->CcMs.push_back(rel(t));
						uint64_t by = owner(e.Src, e.SrcMaster);
						p->CcIn.push_back({rel(t), std::max(0, e.Value), CC_Other, enemy(by)});
						rawCc.push_back({p, p->CcIn.size() - 1, e.Skill, by, t});
					}
					else if (Player* by = !friends.count(e.Dst) ? player(owner(e.Src, e.SrcMaster)) : nullptr)
					{
						by->CcDealt++; // any hostile target, as TopStats counts it
						if (foes.count(e.Dst)) { f.OurCcMs.push_back(rel(t)); }
					}
				}
				if (!IsDamageResult(e.Result)) { break; }
				int64_t dmg = e.Buff ? e.BuffDmg : e.Value;
				if (dmg <= 0) { break; }
				uint64_t o = owner(e.Src, e.SrcMaster);
				int64_t bin = (t - start) / 1000;
				bool inWindow = bin >= 0 && bin < static_cast<int64_t>(seconds);
				if (player(o) && !e.Buff) { hits[o].push_back({t, e.Dst, static_cast<int32_t>(e.Skill)}); }
				if (Player* p = player(o); p && !friends.count(e.Dst))
				{
					p->DamageAll += dmg;
					p->Skills[static_cast<int32_t>(e.Skill)].DamageAll += dmg;
					p->Skills[static_cast<int32_t>(e.Skill)].Hits++;
					if (foes.count(e.Dst))
					{
						p->Damage += dmg; p->Skills[static_cast<int32_t>(e.Skill)].Damage += dmg;
						if (inWindow) { toPlayersPerS[bin] += dmg; }
						p->HitsOut.push_back({rel(t), static_cast<int32_t>(e.Skill), static_cast<int32_t>(dmg), enemy(e.Dst)});
					}
					f.SquadDamage += dmg;
					if (inWindow) { f.OutPerS[bin] += dmg; p->DamagePerS[bin] += static_cast<int32_t>(dmg); }
				}
				else if (Player* hit = player(e.Dst); hit && !friends.count(o))
				{
					hit->DamageTaken += dmg;
					hit->HitsIn.push_back({rel(t), static_cast<int32_t>(e.Skill), static_cast<int32_t>(dmg), enemy(o), e.Buff ? 0 : static_cast<int32_t>(std::min<int64_t>(e.Overstack, dmg))});
					f.GroupDamageTaken[hit->Subgroup] += dmg;
					f.EnemyDamage += dmg;
					if (inWindow) { f.InPerS[bin] += dmg; }
				}
				if (foes.count(e.Src)) { enemiesSeen.insert(e.Src); }
				break;
			}
			case SC_ExtensionCombat:
			{
				if (!(e.Offcycle & kHealSelfReported)) { break; }
				int64_t amount = e.Value < 0 ? -static_cast<int64_t>(e.Value) : e.BuffDmg < 0 ? -static_cast<int64_t>(e.BuffDmg) : 0;
				Player* p = player(owner(e.Src, e.SrcMaster));
				if (!amount || !p || !friends.count(e.Dst)) { break; }
				p->HealKnown = true;
				if (!(e.Offcycle & kHealDowned)) { p->Skills[static_cast<int32_t>(e.Skill)].Hits++; }
				if (e.Value < 0) { heals[p->Addr].push_back({t, e.Dst, static_cast<int32_t>(e.Skill)}); } // direct heals only
				if (int64_t bin = (t - start) / 1000; !(e.Offcycle & kHealDowned) && bin >= 0 && bin < static_cast<int64_t>(seconds)) { f.SupportPerS[bin] += amount; }
				if (e.Shields) { p->Barrier += amount; p->Skills[static_cast<int32_t>(e.Skill)].Barrier += amount; }
				else if (e.Offcycle & kHealDowned) { p->HealDowned += amount; }
				else
				{
					p->Heal += amount;
					p->Skills[static_cast<int32_t>(e.Skill)].Heal += amount;
					if (Player* to = player(e.Dst)) { to->HealsIn.push_back({rel(t), static_cast<int32_t>(amount)}); }
					if (int64_t bin = (t - start) / 1000; bin >= 0 && bin < static_cast<int64_t>(seconds)) { p->HealPerS[bin] += static_cast<int32_t>(amount); }
					const Agent* target = log.Find(e.Dst);
					if (e.Dst == p->Addr) { p->HealToSelf += amount; }
					else if (target && target->Subgroup == p->Subgroup) { p->HealToGroup += amount; }
					else { p->HealToOthers += amount; }
				}
				break;
			}
			case SC_BuffRemoveAll:
			{
				// Illusion of Life ending: ran out (at its full length) or ended early
				if (Player* holder = player(e.Src); holder && isIllusion(e.Skill) && !holder->IllusionOfLife.empty())
				{
					auto& il = holder->IllusionOfLife.back();
					il.RanOut = rel(t) - il.From >= illusionLength[e.Src] - 200;
					il.To = rel(t);
				}
				// the enemy removed one of our boons: a strip on us
				if (Player* holder = player(e.Src); holder && e.Iff == IFF_FOE)
				{
					if (int b = BoonIndex(e.Skill); b >= 0)
					{
						holder->StripsIn.push_back({rel(t), b, enemy(e.Dst), false});
						rawStrips.push_back({holder, holder->StripsIn.size() - 1, e.Dst, t});
						if (b == kStability) { holder->StabStripped++; lastStabStrip[e.Src] = t; holder->StabLost.push_back({rel(t), 0}); }
					}
				}
				// src lost the buff: a signet's passive coming off is the signet being used (not while down or dead)
				// (a use only if it stays off 2 s: the recharge; some signets churn their buff many times a second)
				if (player(e.Src) && !cannotActSince.count(e.Src) && useKind(e.Skill) == U_Signet) { signetOff[e.Src * 1000003ULL ^ e.Skill] = {e.Src, e.Skill, t}; }
				Player* p = player(e.Dst); // dst removed it; its own removals only, not a pet's
				if (!p) { break; }
				if (BoonIndex(e.Skill) >= 0 && e.Iff == IFF_FOE) { p->Strips++; stripTimes[e.Dst].push_back({t, e.Src}); f.OurStripsMs.push_back(rel(t)); p->StripMs.push_back(rel(t)); }
				else if (kConditions.count(e.Skill) && friends.count(e.Src))
				{
					if (e.Src == e.Dst) { p->CleansesSelf++; }
					else { p->Cleanses++; cleanseTimes[e.Dst].push_back({t, e.Src}); f.OurCleansesMs.push_back(rel(t)); p->CleanseMs.push_back(rel(t)); }
				}
				break;
			}
			case SC_AnimationStart:
				if (Player* p = player(e.Src))
				{
					if (e.Skill == kDodge) { p->DodgeMs.push_back(rel(t)); }
					casts[e.Src].push_back({t - start, static_cast<int32_t>(e.Skill)});
					windows[e.Src].push_back({t, t + std::max({e.Value, e.BuffDmg, 0}) + kCastSlack, static_cast<int32_t>(e.Skill), false});
					if (e.Skill == kResurrect && player(e.Dst) && e.Dst != e.Src) { revivingOpen[e.Src] = {t, static_cast<int>(index[e.Dst])}; }
				}
				break;
			case SC_AnimationStop:
				if (Player* p = player(e.Src))
				{
					if (auto it = revivingOpen.find(e.Src); e.Skill == kResurrect && it != revivingOpen.end())
					{
						p->Reviving.push_back({rel(it->second.first), rel(t), it->second.second});
						p->ReviveMs += t - it->second.first;
						revivingOpen.erase(it);
					}
					// a revive skill that went through (a Battle Standard counts on a Paragon only: the user's rule)
					int32_t base = ReviveCastMs(e.Skill);
					if (base && (e.Skill != 14419 || p->Spec == "Paragon"))
					{
						Player::ReviveUse u{rel(t), static_cast<int32_t>(e.Skill)};
						// The stop reason first: control returned = done; a stop the player or the enemy caused (cancel,
						// interrupt, death, downed, CC, the next command, a dodge or move) = not done, however long it ran
						// (an Illusion of Life stopped by a weapon stow at 1319 ms of 1250 gave nobody the buff). Only
						// when the reason says neither, the time spent against the base cast time.
						bool stopped = e.Result == 6 || (e.Result >= 8 && e.Result <= 12) || e.Result == 14 || e.Result == 16 || e.Result == 17;
						u.Done = e.Result == kStopCompleted || (!stopped && e.BuffDmg >= base);
						u.Stop = e.Result;
						p->ReviveUses.push_back(u);
					}
				}
				if (Player* p = player(e.Src); p && e.Activation == kActivationCancel)
				{
					// n_animationstop: 8 interrupt, 9 death, 10 downed, 11 crowd control; the rest the player's own
					bool forced = e.Result >= 8 && e.Result <= 11;
					SkillRow& row = p->Skills[static_cast<int32_t>(e.Skill)];
					(forced ? row.Interrupted : row.Cancelled)++;
				}
				break;
			case SC_WeapSwap:
				if (player(e.Src)) { casts[e.Src].push_back({t - start, kWeaponSwap}); }
				break;
			case SC_ChangeDown:
				if (Player* p = player(e.Src)) { p->Downs++; f.SquadDowns++; cannotActSince.emplace(e.Src, t); downOpen[e.Src] = t; f.SquadDownMs.push_back(rel(t)); }
				else if (foes.count(e.Src)) { f.EnemyDowns++; f.EnemyDownMs.push_back(rel(t)); enemyDownOpen[enemy(e.Src)] = t; }
				break;
			case SC_ChangeDead:
				if (Player* p = player(e.Src))
				{
					p->Deaths++; f.SquadDeaths++; deadSince.emplace(e.Src, t); cannotActSince.emplace(e.Src, t);
					if (auto it = downOpen.find(e.Src); it != downOpen.end()) { p->DownSpans.push_back({rel(it->second), rel(t), false}); downOpen.erase(it); }
					deadOpen[e.Src] = t;
				}
				else if (foes.count(e.Src))
				{
					f.EnemyDeaths++;
					int en = enemy(e.Src);
					if (auto it = enemyDownOpen.find(en); it != enemyDownOpen.end()) { f.Enemies[en].DownSpans.push_back({rel(it->second), rel(t), false}); enemyDownOpen.erase(it); }
					enemyDeadOpen[en] = t;
				}
				break;
			case SC_ChangeUp: case SC_Spawn:
				if (auto it = deadSince.find(e.Src); it != deadSince.end()) { deadMs[e.Src] += t - it->second; deadSince.erase(it); }
				if (auto it = cannotActSince.find(e.Src); it != cannotActSince.end()) { cannotAct[e.Src].push_back({it->second, t}); cannotActSince.erase(it); }
				if (Player* p = player(e.Src))
				{
					if (auto it = downOpen.find(e.Src); it != downOpen.end()) { p->DownSpans.push_back({rel(it->second), rel(t), false}); downOpen.erase(it); }
					if (auto it = deadOpen.find(e.Src); it != deadOpen.end()) { p->DownSpans.push_back({rel(it->second), rel(t), true}); deadOpen.erase(it); }
				}
				else if (foes.count(e.Src))
				{
					int en = enemy(e.Src);
					if (auto it = enemyDownOpen.find(en); it != enemyDownOpen.end()) { f.Enemies[en].DownSpans.push_back({rel(it->second), rel(t), false}); enemyDownOpen.erase(it); }
					if (auto it = enemyDeadOpen.find(en); it != enemyDeadOpen.end()) { f.Enemies[en].DownSpans.push_back({rel(it->second), rel(t), true}); enemyDeadOpen.erase(it); }
				}
				break;
			case SC_BuffApply: case SC_BuffInitial:
			{
				// a profession mechanic switched on: its buff of the same name on the player
				if (e.StateChange == SC_BuffApply && e.Src == e.Dst && player(e.Src) && useKind(e.Skill) == U_Mode) { noteUse(e.Src, e.Skill, t); }
				if (e.Src == e.Dst)
				{
					auto it = legendOf.find(e.Skill);
					if (it == legendOf.end()) { it = legendOf.emplace(e.Skill, LegendOf(log.SkillName(e.Skill))).first; }
					if (Player* p = it->second ? player(e.Dst) : nullptr) { p->Legends |= it->second; }
				}
				// on one of ours: a control effect (for the CC's type), a condition (for corruptions)
				if (e.StateChange == SC_BuffApply && player(e.Dst))
				{
					if (isIllusion(e.Skill))
					{
						Player* by = player(e.Src);
						player(e.Dst)->IllusionOfLife.push_back({rel(t), rel(t + std::max(0, e.Value)), by ? static_cast<int>(by - f.Players.data()) : -1, false});
						illusionLength[e.Dst] = std::max(0, e.Value);
					}
					auto it = controlOf.find(e.Skill);
					if (it == controlOf.end())
					{
						const std::string name = log.SkillName(e.Skill);
						int kind = name == "Stun" ? CC_Stun : name == "Daze" ? CC_Daze : name == "Fear" ? CC_Fear : name == "Taunt" ? CC_Taunt : -1;
						it = controlOf.emplace(e.Skill, kind).first;
					}
					if (it->second >= 0) { controlApplied[e.Dst].push_back({t, static_cast<CcKind>(it->second)}); }
					if (kConditions.count(e.Skill)) { condApplied[e.Dst].push_back({t, e.Src}); }
				}
				if (e.StateChange == SC_BuffApply && player(e.Dst))
				{
					auto off = signetOff.find(e.Dst * 1000003ULL ^ e.Skill); // a signet's passive back: was it off long enough?
					if (off != signetOff.end())
					{
						if (t - off->second.Time >= 2000) { noteUse(off->second.Player, off->second.Buff, off->second.Time); }
						signetOff.erase(off);
					}
				}
				int b = BoonIndex(e.Skill);
				if (b < 0 || !player(e.Dst)) { break; }
				if (e.StateChange == SC_BuffApply && player(e.Src))
				{
					boonApplied[e.Src][b].push_back(t);
					if (e.Src != e.Dst && e.Value > 0) { boonDurations[e.Src][b].push_back(e.Value); }
				}
				bool counting = f.Intensity[b] || e.Shields;
				uint64_t key = stackKey(e.Dst, e.Pad61);
				stop(key, e.Dst, t);
				stacks[key] = {e.Src, b, t, t, counting, e.StateChange == SC_BuffApply ? std::max(0, e.Value) : 0};
				stackTarget[key] = e.Dst;
				if (b == kStability)
				{
					if (auto old = stabIndex.find(key); old != stabIndex.end()) { stab[old->second].Removed = std::min(stab[old->second].Removed, t); }
					stabIndex[key] = stab.size();
					stab.push_back({e.Src, e.Dst, t, INT64_MAX, t + std::max(0, e.Value)});
				}
				break;
			}
			case SC_BuffActive:
			{
				auto it = stacks.find(stackKey(e.Src, static_cast<uint32_t>(e.Dst)));
				if (it != stacks.end() && !it->second.Counting) { it->second.Counting = true; it->second.Since = t; }
				break;
			}
			case SC_BuffDeactive:
			{
				uint64_t key = stackKey(e.Src, e.Pad61);
				auto it = stacks.find(key);
				if (it != stacks.end() && !f.Intensity[it->second.Boon]) { stop(key, e.Src, t); }
				break;
			}
			case SC_BuffChange:
			{
				// a stack's duration changed (extended or cut): overstack_value is its new remaining ms
				auto it = stabIndex.find(stackKey(e.Dst, e.Pad61));
				if (e.Skill == kBoonIds[kStability] && it != stabIndex.end()) { stab[it->second].NominalEnd = t + static_cast<int64_t>(e.Overstack); }
				break;
			}
			case SC_BuffRemoveSingle:
			{
				uint64_t key = stackKey(e.Src, e.Pad61);
				if (auto it = stabIndex.find(key); it != stabIndex.end() && e.Skill == kBoonIds[kStability])
				{
					stab[it->second].Removed = std::min(stab[it->second].Removed, t);
					// taken by the enemy: part of a strip (right after a remove-all), else used up one at a time
					if (e.Iff == IFF_FOE)
					{
						auto ls = lastStabStrip.find(e.Src);
						bool partOfStrip = ls != lastStabStrip.end() && t - ls->second <= 5;
						if (Player* holder = player(e.Src); holder && !partOfStrip) { holder->StabUsedUp++; holder->StabLost.push_back({rel(t), 1}); }
						if (Player* giver = player(stab[it->second].Source)) { giver->StabGivenLost++; }
					}
					stabIndex.erase(it);
				}
				if (stacks.count(key)) { stop(key, e.Src, t); stacks.erase(key); }
				break;
			}
			case SC_HealthPct:
				if (Player* p = player(e.Src)) { p->Hp.push_back({rel(t), static_cast<int32_t>(e.Dst)}); }
				else if (foes.count(e.Src)) { f.Enemies[enemy(e.Src)].Hp.push_back({rel(t), static_cast<int32_t>(e.Dst)}); }
				break;
			case SC_Position:
				if (Player* p = player(e.Src); p && (p->Pos.empty() || rel(t) - p->Pos.back().Ms >= 250))
				{
					float xy[2];
					std::memcpy(xy, &e.Dst, sizeof(xy));
					p->Pos.push_back({rel(t), xy[0], xy[1]});
				}
				else if (int en = enemy(e.Src); en >= 0 && (f.Enemies[en].Pos.empty() || rel(t) - f.Enemies[en].Pos.back().Ms >= 250))
				{
					float xy[2];
					std::memcpy(xy, &e.Dst, sizeof(xy));
					f.Enemies[en].Pos.push_back({rel(t), xy[0], xy[1]});
				}
				break;
			case SC_Teleport:
				if (Player* p = player(e.Src)) { p->TeleportMs.push_back(rel(t)); }
				break;
			case SC_Marker:
				if (e.Buff && e.Value != 0 && player(e.Src)) { commander = e.Src; }
				break;
			default:
				break;
			}
		}
		for (auto& [key, st] : stacks) { stop(key, stackTarget[key], end); }
		for (auto& [en, since] : enemyDownOpen) { f.Enemies[en].DownSpans.push_back({rel(since), rel(end), false}); }
		for (auto& [en, since] : enemyDeadOpen) { f.Enemies[en].DownSpans.push_back({rel(since), rel(end), true}); }

		// CC types (Elite Insights' ArcDPSGeneric ids): a stun or daze by the effect applied with it (within 60 ms), a
		// knockback or pull by whether the player ended up 150+ nearer the enemy who did it (their positions 0.1 s before
		// and 1 s after)
		auto posNear = [](const std::vector<Player::Point>& aPos, int32_t aMs) -> const Player::Point*
		{
			auto it = std::lower_bound(aPos.begin(), aPos.end(), aMs, [](const Player::Point& a, int32_t ms) { return a.Ms < ms; });
			const Player::Point* best = nullptr;
			if (it != aPos.end()) { best = &*it; }
			if (it != aPos.begin() && (!best || aMs - (it - 1)->Ms < best->Ms - aMs)) { best = &*(it - 1); }
			return best && std::abs(best->Ms - aMs) <= 400 ? best : nullptr;
		};
		for (const RawCc& r : rawCc)
		{
			Player::CcHit& h = r.P->CcIn[r.Index];
			auto effect = [&](CcKind aKind)
			{
				for (auto& [at, k] : controlApplied[r.P->Addr]) { if (k == aKind && std::llabs(at - r.T) <= 60) { return true; } }
				return false;
			};
			switch (r.Type)
			{
			case 23294: h.Kind = CC_Knockdown; break;
			case 23296: case 23298: case 23304: case 23305: h.Kind = CC_Float; break;
			case 23297: h.Kind = CC_Launch; break;
			case 23300: h.Kind = CC_Stagger; break;
			case 23307: h.Kind = CC_Fear; break;
			case 23306: h.Kind = effect(CC_Daze) ? CC_Daze : effect(CC_Stun) ? CC_Stun : CC_StunOrDaze; break;
			case 23299: h.Kind = effect(CC_Taunt) ? CC_Taunt : effect(CC_Fear) ? CC_Fear : effect(CC_Stun) ? CC_Stun : effect(CC_Daze) ? CC_Daze : CC_Other; break;
			case 23295:
			{
				h.Kind = CC_KnockbackOrPull;
				if (h.Enemy < 0) { break; }
				const auto* a = posNear(r.P->Pos, h.Ms - 100);
				const auto* b = posNear(r.P->Pos, h.Ms + 1000);
				const auto* by = posNear(f.Enemies[h.Enemy].Pos, h.Ms);
				if (!a || !b || !by) { break; }
				double before = std::hypot(a->X - by->X, a->Y - by->Y), after = std::hypot(b->X - by->X, b->Y - by->Y);
				if (after <= before - 150) { h.Kind = CC_Pull; }
				else if (after >= before + 150) { h.Kind = CC_Knockback; }
				break;
			}
			default: h.Kind = CC_Other; break;
			}
		}
		// Corrupted: a condition from the same enemy within 10 ms of the boon's removal
		for (const RawStrip& r : rawStrips)
		{
			for (auto& [at, by] : condApplied[r.P->Addr]) { if (by == r.By && std::llabs(at - r.T) <= 10) { r.P->StripsIn[r.Index].Corrupted = true; break; } }
		}
		// Down contribution (Elite Insights): damage to an enemy from their last moment at 90%+ health to a down that led
		// to their death
		{
			std::vector<std::vector<std::pair<int32_t, int32_t>>> fatal(f.Enemies.size()); // enemy -> (from, down)
			for (size_t en = 0; en < f.Enemies.size(); en++)
			{
				const auto& spans = f.Enemies[en].DownSpans;
				for (size_t i = 0; i + 1 < spans.size(); i++)
				{
					if (spans[i].Dead || !spans[i + 1].Dead || spans[i + 1].From != spans[i].To) { continue; }
					int32_t from = -1;
					for (auto& [ms, hp] : f.Enemies[en].Hp) { if (ms > spans[i].From) { break; } if (hp > 9000) { from = ms; } }
					if (from >= 0) { fatal[en].push_back({from, spans[i].From}); }
				}
			}
			for (Player& p : f.Players)
			{
				for (const auto& h : p.HitsOut)
				{
					if (h.Enemy < 0) { continue; }
					for (auto& [a, b] : fatal[h.Enemy]) { if (h.Ms >= a && h.Ms <= b) { p.DownContribution += h.Damage; break; } }
				}
			}
		}
		for (auto& [key, off] : signetOff) { if (end - off.Time >= 2000) { noteUse(off.Player, off.Buff, off.Time); } }
		for (auto& [caster, found] : InstantCasts::Find(log))
		{
			if (!player(caster)) { continue; }
			for (auto& [t, skill] : found)
			{
				casts[caster].push_back({t - start, skill});
				windows[caster].push_back({t - kInstantSlack, t + kInstantSlack, skill, true});
			}
		}
		for (auto& [addr, since] : deadSince) { deadMs[addr] += end - since; }
		f.EnemyCount = static_cast<int>(enemiesSeen.size());

		// Spikes and timing
		// Our spikes from damage to enemy players only (not NPCs, siege or gates). On 110 rounds of 30 s+ that the
		// settings weren't tuned on: 63% of our spikes had an enemy down within 1 s before to 4 s after, and 68% of
		// enemy downs fell in one of our spikes; enemy spikes 59% and 73% (the old settings, radius 3 s, 1.6x the
		// median, 35% of the biggest second: 59% / 59% and 59% / 62%). 2 s sums scored no better.
		f.OurSpikesMs = Spikes(toPlayersPerS);
		f.ToPlayersPerS = std::move(toPlayersPerS);
		f.TheirSpikesMs = Spikes(f.InPerS);
		for (size_t s = 0; s < seconds; s++)
		{
			auto c = Classify(static_cast<int64_t>(s) * 1000 + 500, f);
			for (int k = 0; k < T_Count; k++) { f.TimingBaseline[k] += c[k] ? 1.0 / seconds : 0.0; }
		}

		for (Player& p : f.Players)
		{
			p.ActiveMs = std::max<int64_t>(0, f.DurationMs - deadMs[p.Addr]);
			for (auto& [tm, skill] : casts[p.Addr])
			{
				SkillRow& row = p.Skills[skill];
				row.Casts++;
				row.CastMs.push_back(static_cast<int32_t>(tm));
				auto c = Classify(tm, f);
				for (int k = 0; k < T_Count; k++) { row.Timing[k] += c[k]; }
			}
			auto& w = windows[p.Addr];
			std::sort(w.begin(), w.end(), [](const Window& x, const Window& y) { return x.Start < y.Start; });
			static const std::unordered_set<int32_t> cleansers(std::begin(kCleanseSkills), std::end(kCleanseSkills));
			static const std::unordered_set<int32_t> strippers(std::begin(kStripSkills), std::end(kStripSkills));
			for (auto& [t, target] : stripTimes[p.Addr]) { p.Skills[AttributeRemoval(hits[p.Addr], t, target, w, strippers)].Strips++; }
			for (auto& [t, target] : cleanseTimes[p.Addr]) { p.Skills[AttributeRemoval(heals[p.Addr], t, target, w, cleansers)].Cleanses++; }
		}

		// Uses shown only by a skill's first hit or heal (instant skills without a cast event, and pulses of a field
		// after a gap of 2 s): not counted as casts, but a boon at that moment can come from them
		for (Player& p : f.Players)
		{
			std::unordered_set<int32_t> cast;
			for (auto& [tm, skill] : casts[p.Addr]) { cast.insert(skill); }
			std::vector<Hit> seen = hits[p.Addr];
			seen.insert(seen.end(), heals[p.Addr].begin(), heals[p.Addr].end());
			std::sort(seen.begin(), seen.end(), [](const Hit& x, const Hit& y) { return x.Time < y.Time; });
			std::unordered_map<int32_t, int64_t> last;
			auto& w = windows[p.Addr];
			for (const Hit& h : seen)
			{
				if (h.Skill == 0 || cast.count(h.Skill)) { continue; }
				auto it = last.find(h.Skill);
				if (it == last.end() || h.Time - it->second > 2000) { w.push_back({h.Time - kInstantSlack, h.Time + kInstantSlack, h.Skill, true}); }
				last[h.Skill] = h.Time;
			}
			std::sort(w.begin(), w.end(), [](const Window& x, const Window& y) { return x.Start < y.Start; });
		}

		// Which skills give which boons, from this round's log and the other rounds': a skill gives a boon when the
		// player applied it within most of the skill's uses, far more often than a random moment would show. Judged
		// on the player's own uses when there are enough (their build), else pooled over everyone of the same
		// profession (sister specs share mechanics).
		std::unordered_map<uint64_t, Givers> learned; // player address -> skill -> boon bits
		std::unordered_map<uint32_t, Reach> reach;    // profession -> pulsing skills
		{
			Evidence& evidence = f.OwnEvidence;
			for (Player& p : f.Players)
			{
				const auto& w = windows[p.Addr];
				if (w.empty()) { continue; }
				auto& applied = boonApplied[p.Addr];
				double len = 0;
				for (const Window& x : w) { len += static_cast<double>(x.End - x.Start + kBoonDelay); }
				len = std::max(1.0, len / w.size());
				std::array<double, kBoons> chance{}, perSecond{};
				std::array<std::vector<int64_t>, kBoons> moments; // applications 100+ ms apart (a pulse to 5 allies is one)
				for (int b = 0; b < kBoons; b++)
				{
					std::sort(applied[b].begin(), applied[b].end());
					for (size_t i = 0; i < applied[b].size(); i++) { if (i == 0 || applied[b][i] - applied[b][i - 1] > 100) { moments[b].push_back(applied[b][i]); } }
					chance[b] = std::min(1.0, moments[b].size() * len / std::max<int64_t>(1, f.DurationMs));
					perSecond[b] = moments[b].size() * 1000.0 / std::max<int64_t>(1, f.DurationMs);
				}
				for (const Window& x : w)
				{
					if (x.Skill <= 0 && x.Skill > -20) { continue; } // 0 (other sources), weapon swap; negative ids below are EI's
					if (BoonIndex(static_cast<uint32_t>(x.Skill)) >= 0 || kConditions.count(static_cast<uint32_t>(x.Skill))) { continue; } // a boon isn't its own source
					BoonEvidence& prof = evidence.ByProfession[GiverKey(p.ProfId, x.Skill)];
					for (int b = 0; b < kBoons; b++)
					{
						for (int k = 0; k < kLagSeconds; k++)
						{
							int64_t a = x.End + k * 1000;
							auto lo = std::lower_bound(moments[b].begin(), moments[b].end(), a);
							auto hi = std::lower_bound(lo, moments[b].end(), a + 1000);
							prof.After[b][k] += static_cast<int>(hi - lo);
							prof.AfterExpected[b][k] += perSecond[b];
						}
					}
					for (BoonEvidence* ev : {&prof, &evidence.ByPlayer[{p.Account, x.Skill}]})
					{
						ev->Uses++;
						for (int b = 0; b < kBoons; b++)
						{
							// during the use or shortly after (some skills grant their boons a moment later)
							auto it = std::lower_bound(applied[b].begin(), applied[b].end(), x.Start);
							ev->Followed[b] += it != applied[b].end() && *it <= x.End + kBoonDelay;
							ev->Expected[b] += chance[b] * (x.End + kBoonDelay - x.Start) / len;
						}
					}
				}
			}
			Evidence pooled = aOthers && aOthersHaveThis ? *aOthers : evidence;
			if (aOthers && !aOthersHaveThis) { AddEvidence(pooled, *aOthers); }
			constexpr int kEnoughUses = 3;
			auto judge = [](const BoonEvidence& ev)
			{
				uint32_t mask = 0;
				for (int b = 0; b < kBoons; b++)
				{
					if (ev.Uses >= kEnoughUses && ev.Followed[b] * 2 >= ev.Uses && ev.Followed[b] >= 3 * ev.Expected[b] + 1) { mask |= 1u << b; }
				}
				return mask;
			};
			// Pulses: a skill that gives a boon keeps giving it for as many seconds after the use as the boon still
			// comes at least twice as often as usual (5+ uses)
			for (auto& [key, ev] : pooled.ByProfession)
			{
				if (ev.Uses < 5) { continue; }
				std::array<int32_t, kBoons> r{};
				bool any = false;
				for (int b = 0; b < kBoons; b++)
				{
					int k = 1;
					while (k < kLagSeconds && ev.After[b][k] >= 2 * ev.AfterExpected[b][k] + 2) { k++; }
					if (k > 1) { r[b] = k * 1000; any = true; f.Pulses.push_back({static_cast<uint32_t>(key >> 32), static_cast<int32_t>(key & 0xFFFFFFFF), b, r[b]}); }
				}
				if (any) { reach[static_cast<uint32_t>(key >> 32)][static_cast<int32_t>(key & 0xFFFFFFFF)] = r; }
			}
			for (const Player& p : f.Players)
			{
				Givers& mine = learned[p.Addr];
				// the profession's pool first, then the player's own uses where there are enough of them
				for (auto it = pooled.ByProfession.lower_bound(GiverKey(p.ProfId, 0)); it != pooled.ByProfession.end() && (it->first >> 32) == p.ProfId; ++it)
				{
					if (uint32_t mask = judge(it->second) & ~NotGiven(static_cast<int32_t>(it->first & 0xFFFFFFFF))) { mine[static_cast<int32_t>(it->first & 0xFFFFFFFF)] = mask; }
				}
				for (auto it = pooled.ByPlayer.lower_bound({p.Account, INT32_MIN}); it != pooled.ByPlayer.end() && it->first.first == p.Account; ++it)
				{
					if (it->second.Uses >= kEnoughUses) { mine[it->first.second] = judge(it->second) & ~NotGiven(it->first.second); }
				}
				for (auto& [skill, mask] : mine) { if (mask) { f.LearnedGivers.push_back({p.Account, p.ProfId, skill, mask}); } }
			}
		}

		// Boon generation and uptime
		std::unordered_map<uint64_t, std::array<double, kBoons>> onTarget;
		std::map<std::pair<uint64_t, int>, double> durationScale; // (player, boon) -> scale; -1: no fit
		struct Give { Player* Src; int32_t Ms; int32_t Skill; int Target; };
		std::vector<Give> gives; // stability given, one entry per stack reaching someone
		for (const Credit& c : credits)
		{
			onTarget[c.Target][c.Boon] += c.Ms;
			if (Player* on = player(c.Target)) { on->BoonOn[c.Boon].push_back({rel(c.From), rel(c.From + c.Ms)}); }
			Player* src = player(c.Source);
			Player* tgt = player(c.Target);
			if (!src || !tgt) { continue; }
			double& scale = durationScale[{src->Addr, c.Boon}];
			if (scale == 0) { scale = DurationScale(src->Spec, c.Boon, boonDurations[src->Addr][c.Boon]); if (scale == 0) { scale = -1; } }
			int32_t skill = SourceByDuration(src->Spec, c.Boon, c.Duration, scale);
			if (!skill) { skill = AttributeBoon(windows[src->Addr], c.Applied, c.Boon, learned[src->Addr], reach[src->ProfId]); }
			if (c.Boon == kStability) { gives.push_back({src, rel(c.Applied), skill, static_cast<int>(tgt - f.Players.data())}); }
			if (src == tgt) { continue; }
			double s = c.Ms / 1000.0;
			src->BoonSquadS[c.Boon] += s;
			src->Skills[skill].BoonSquadS[c.Boon] += s;
			if (src->Subgroup == tgt->Subgroup)
			{
				src->BoonGroupS[c.Boon] += s;
				src->Skills[skill].BoonGroupS[c.Boon] += s;
			}
		}
		// Stability given, as moments: the stacks of one cast or pulse (all allies, all stacks) within 150 ms are one
		std::sort(gives.begin(), gives.end(), [](const Give& a, const Give& b) { return a.Src != b.Src ? a.Src < b.Src : a.Ms < b.Ms; });
		for (const Give& g : gives)
		{
			// the moment of the same skill within 150 ms, else a new one (two skills can alternate stack by stack: opening
			// Tome of Courage and its Indomitable Courage trait at the same ms)
			auto& list = g.Src->StabGives;
			Player::StabGive* into = nullptr;
			// (a stack credited to no skill joins a named one at the same moment: it came from the same cast)
			for (auto it = list.rbegin(); it != list.rend() && g.Ms - it->Ms <= 150; ++it)
			{
				if (it->Skill == g.Skill || g.Skill == 0 || it->Skill == 0) { into = &*it; if (into->Skill == 0) { into->Skill = g.Skill; } break; }
			}
			if (!into) { list.push_back({g.Ms, g.Skill, {}}); into = &list.back(); }
			auto& targets = into->Targets;
			if (std::find(targets.begin(), targets.end(), g.Target) == targets.end()) { targets.push_back(g.Target); }
		}
		for (const Player& p : f.Players)
		{
			for (int key : {0, p.Subgroup})
			{
				auto& up = f.GroupUptime[key];
				f.GroupSize[key]++;
				for (int b = 0; b < kBoons; b++) { up[b] += onTarget[p.Addr][b]; }
			}
		}
		for (auto& [g, up] : f.GroupUptime)
		{
			for (int b = 0; b < kBoons; b++)
			{
				double v = up[b] / (static_cast<double>(f.DurationMs) * f.GroupSize[g]);
				up[b] = f.Intensity[b] ? v : 100.0 * v;
			}
		}

		// Stability performance, TopStats' definitions (reference/topstats_stability_guide.txt)
		for (auto& [addr, since] : cannotActSince) { cannotAct[addr].push_back({since, end}); }
		for (auto& [addr, since] : downOpen) { if (Player* p = player(addr)) { p->DownSpans.push_back({rel(since), rel(end), false}); } }
		for (auto& [addr, since] : deadOpen) { if (Player* p = player(addr)) { p->DownSpans.push_back({rel(since), rel(end), true}); } }
		for (StabStack& s : stab)
		{
			s.Removed = std::min(s.Removed, end);
			Player* src = player(s.Source);
			if (!src || s.Removed <= s.Applied) { continue; }
			int64_t ms = std::min(s.Removed, end) - std::max(s.Applied, start);
			if (ms <= 0) { continue; }
			(s.Source == s.Target ? src->StabSelfMs : src->StabAllyMs) += ms;
		}
		std::unordered_map<uint64_t, std::vector<const StabStack*>> stabOn; // target -> stacks
		for (const StabStack& s : stab) { stabOn[s.Target].push_back(&s); }
		for (Player& p : f.Players)
		{
			for (const Span& s : p.DownSpans) { if (!s.Dead) { p.DownedMs += s.To - s.From; } }
			std::vector<std::pair<int32_t, int32_t>> on;
			for (const StabStack* s : stabOn[p.Addr])
			{
				int32_t a = rel(s->Applied), b = rel(s->Removed);
				if (b > a) { on.push_back({a, b}); }
			}
			std::sort(on.begin(), on.end());
			for (auto& span : on)
			{
				if (!p.StabOnMe.empty() && span.first <= p.StabOnMe.back().second) { p.StabOnMe.back().second = std::max(p.StabOnMe.back().second, span.second); }
				else { p.StabOnMe.push_back(span); }
			}
			// A CC that lands means no stability at that moment; what tells is whether there was any just before
			for (int32_t c : p.CcMs)
			{
				bool had = false;
				for (auto& [a, b] : p.StabOnMe) { if (a <= c && b >= c - 3000) { had = true; break; } }
				p.CcNoStab += !had;
			}
		}
		auto canAct = [&](uint64_t aPlayer, int64_t a, int64_t b)
		{
			for (auto& [from, to] : cannotAct[aPlayer]) { if (from <= a && to >= b) { return false; } }
			return true;
		};
		for (auto& [recipient, contacts] : ccContacts)
		{
			const Player* r = player(recipient);
			if (!r) { continue; }
			std::sort(contacts.begin(), contacts.end());
			int64_t windowEnd = INT64_MIN;
			for (int64_t t : contacts)
			{
				if (t < windowEnd) { continue; } // later contacts don't extend a window
				windowEnd = t + kCcWindowMs;
				f.GroupCcWindows[r->Subgroup]++;
				for (const StabStack* s : stabOn[recipient])
				{
					if (s->Applied <= windowEnd && s->NominalEnd >= t) { f.GroupCcCovered[r->Subgroup]++; break; }
				}
				for (Player& p : f.Players)
				{
					if (p.Subgroup != r->Subgroup || p.Addr == recipient || !canAct(p.Addr, t, windowEnd)) { continue; }
					p.StabEligible++;
					bool covered = false, ready = false;
					for (const StabStack* s : stabOn[recipient])
					{
						// TopStats counts a stack as there from its application to its planned end (NominalEnd), even
						// when a CC used it up earlier: Coverage is "did you give stability that should still run"
						if (s->Source != p.Addr || s->Applied > windowEnd || s->NominalEnd < t) { continue; }
						covered = true;
						if (s->NominalEnd - std::max(t, s->Applied) >= kReadyMs) { ready = true; }
					}
					p.StabCovered += covered;
					p.StabReady += ready;
				}
			}
		}

		// Boon spans merged per player; the commander
		for (Player& p : f.Players)
		{
			for (auto& spans : p.BoonOn)
			{
				std::sort(spans.begin(), spans.end());
				std::vector<std::pair<int32_t, int32_t>> merged;
				for (auto& s : spans)
				{
					if (!merged.empty() && s.first <= merged.back().second) { merged.back().second = std::max(merged.back().second, s.second); }
					else { merged.push_back(s); }
				}
				spans.swap(merged);
			}
		}
		if (auto it = index.find(commander); commander && it != index.end()) { f.Commander = it->second; }

		// Revive skills: how many allies were downed within reach when it went off, and how many of them got up
		// within 3 s (a down span that ends without a death is a get-up)
		auto posAt = [](const Player& p, int32_t aMs) -> const Player::Point*
		{
			auto it = std::lower_bound(p.Pos.begin(), p.Pos.end(), aMs, [](const Player::Point& a, int32_t ms) { return a.Ms < ms; });
			if (it == p.Pos.end()) { return p.Pos.empty() ? nullptr : &p.Pos.back(); }
			if (it != p.Pos.begin() && aMs - (it - 1)->Ms < it->Ms - aMs) { --it; }
			return &*it;
		};
		for (Player& p : f.Players)
		{
			for (auto& u : p.ReviveUses)
			{
				if (!u.Done) { continue; }
				const Player::Point* me = posAt(p, u.Ms);
				for (const Player& q : f.Players)
				{
					if (&q == &p) { continue; }
					const Player::Point* them = posAt(q, u.Ms);
					if (!me || !them || std::hypot(me->X - them->X, me->Y - them->Y) > kReviveReach) { continue; }
					for (size_t i = 0; i < q.DownSpans.size(); i++)
					{
						const Span& s = q.DownSpans[i];
						if (s.Dead || s.From > u.Ms || s.To < u.Ms) { continue; }
						u.DownNear++;
						bool died = i + 1 < q.DownSpans.size() && q.DownSpans[i + 1].Dead && q.DownSpans[i + 1].From == s.To;
						if (!died && s.To <= u.Ms + 3000) { u.GotUp++; }
					}
				}
			}
		}

		// Names for every skill a player has a row for, and for the enemy skills that hit them
		for (const Player& p : f.Players)
		{
			for (const auto& h : p.HitsIn)
			{
				if (f.SkillNames.count(h.Skill)) { continue; }
				std::string name = log.SkillName(static_cast<uint32_t>(h.Skill));
				if (const char* ei = InstantCasts::Name(h.Skill, false); ei && name.empty()) { name = ei; }
				f.SkillNames[h.Skill] = name.empty() ? std::to_string(h.Skill) : name;
			}
			for (auto& [skill, row] : p.Skills)
			{
				if (f.SkillNames.count(skill)) { continue; }
				std::string name;
				for (const DurationSource& d : kDurationSources) { if (d.Skill == skill) { name = d.Name; } }
				if (!name.empty()) { f.SkillNames[skill] = name; continue; }
				switch (skill)
				{
				case 0: name = "other sources (traits, relics, sigils, runes, combos)"; break;
				case kWeaponSwap: name = "Weapon Swap"; break;
				case 23275: name = "Dodge"; break;
				case 23285: name = "Weapon Stow"; break;
				case 1066: name = "Resurrect"; break;
				default:
					if (const char* ei = InstantCasts::Name(skill, false)) { name = ei; }
					else { name = log.SkillName(skill); }
					if (name.empty()) { if (const char* fb = InstantCasts::Name(skill, true)) { name = fb; } }
					break;
				}
				if (name.empty()) { name = std::to_string(skill); }
				f.SkillNames[skill] = name;
			}
		}
		// the duration table's names win (the log names some of those skills differently, or not at all)
		for (const DurationSource& d : kDurationSources) { if (f.SkillNames.count(d.Skill)) { f.SkillNames[d.Skill] = d.Name; } }
		return f;
	}
}
