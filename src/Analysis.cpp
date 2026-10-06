#include "Analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
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

		// A spike: a run of seconds at or over the floor (1.4x the round's median busy second, or half its biggest, whichever
		// is more), a 1 s dip bridged while it stays over the median; its peak the run's highest second (the user,
		// 2026-10-06: the enemy's 12 s assault at 22:00 showed as three spikes, peaks 3 s apart). Scored against downs on
		// 166 rounds (scratch merge_eval.py on data/spike_series_*.tsv): precision as before (ours 0.63, theirs 0.59),
		// recall 0.80 / 0.85 against 0.68 / 0.73; 3.8 / 4.1 a minute against 4.2 / 4.5; 2.6 s long on average
		struct SpikeRun { int64_t Peak = 0; int32_t From = 0, To = 0; };
		std::vector<SpikeRun> Spikes(const std::vector<int64_t>& aSeries)
		{
			std::vector<int64_t> busy;
			for (int64_t v : aSeries) { if (v > 0) { busy.push_back(v); } }
			std::vector<SpikeRun> out;
			if (busy.empty()) { return out; }
			std::sort(busy.begin(), busy.end());
			double median = busy.size() % 2 ? busy[busy.size() / 2] : (busy[busy.size() / 2 - 1] + busy[busy.size() / 2]) / 2.0;
			double floor = std::max(kSpikeOverMedian * median, kSpikeOfMax * busy.back());
			int64_t a = -1, b = -1; // the run being built: first and last second
			auto close = [&]()
			{
				if (a < 0) { return; }
				int64_t top = a;
				for (int64_t i = a; i <= b; i++) { if (aSeries[i] > aSeries[top]) { top = i; } }
				out.push_back({top * 1000 + 500, static_cast<int32_t>(a * 1000), static_cast<int32_t>((b + 1) * 1000)});
				a = b = -1;
			};
			for (int64_t i = 0; i < static_cast<int64_t>(aSeries.size()); i++)
			{
				if (aSeries[i] < floor) { continue; }
				const bool joins = a >= 0 && (b == i - 1 || (b == i - 2 && aSeries[i - 1] >= median));
				if (!joins) { close(); a = i; }
				b = i;
			}
			close();
			return out;
		}

		// A spike's first and last seconds' middles (from fight start): its peak for a one-second spike
		std::pair<int64_t, int64_t> RunOf(const std::vector<int64_t>& aPeaks, const std::vector<std::pair<int32_t, int32_t>>& aSpans, size_t i)
		{
			if (i >= aSpans.size()) { return {aPeaks[i], aPeaks[i]}; }
			return {std::min<int64_t>(aPeaks[i], aSpans[i].first + 500), std::max<int64_t>(aPeaks[i], aSpans[i].second - 500)};
		}

		// The timing classes, against each spike's run (2026-10-06; the same as against the peak for a one-second spike):
		// into ours from 2 s before its run to 2 s after, ahead of theirs in the 4 s before its run starts, answering theirs
		// from its start to 3 s after its end
		std::array<bool, T_Count> Classify(int64_t aT, const Fight& aFight)
		{
			std::array<bool, T_Count> c{};
			for (size_t i = 0; i < aFight.OurSpikesMs.size(); i++)
			{
				auto [first, last] = RunOf(aFight.OurSpikesMs, aFight.OurSpikeSpans, i);
				if (aT >= first - kIntoOurs && aT <= last + kIntoOurs) { c[T_IntoOurs] = true; }
			}
			for (size_t i = 0; i < aFight.TheirSpikesMs.size(); i++)
			{
				auto [first, last] = RunOf(aFight.TheirSpikesMs, aFight.TheirSpikeSpans, i);
				if (first - aT > 0 && first - aT <= kAheadOfTheirs) { c[T_AheadOfTheirs] = true; }
				if (aT >= first && aT <= last + kAnsweringTheirs) { c[T_AnsweringTheirs] = true; }
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
		struct StabStack { uint64_t Source, Target; int64_t Applied, Removed, NominalEnd; int64_t ByFoe = -1; }; // ByFoe: when the enemy took it
		constexpr int64_t kCcWindowMs = 750;   // TopStats: a CC contact opens a 0.75 s window (stability's cooldown)
		constexpr int64_t kReadyMs = 3000;     // TopStats: "Ready" = 3+ s of stability left
		constexpr int64_t kConsumedMs = 50;    // a stack removed this close before a CC contact was consumed by it
		constexpr int64_t kSameMoment = 10; // ms: Elite Insights' server delay

#include "RemovalSkills.inc"
#include "SkillSlots.inc"
#include "TraitBoons.inc"

		// A boon to an ally that no skill explains, put to a trait of the giver's profession and specialization that
		// gives it, when the log shows the trait's trigger at that moment: a dodge, a weapon swap, a heal or elite
		// skill, a skill of the kind it names (a shout, a mantra's stability, a shatter), the giver granting the boons
		// it names; else, when exactly one such trait gives it each interval, that one. 0 when none fits.
		// aCasts: the giver's casts (ms from fight start, skill); aGranted: the giver's boon applications (log time).
		int32_t TraitFor(const Player& aSrc, int64_t aT, int64_t aStart, int aBoon, const std::vector<std::pair<int64_t, int32_t>>& aCasts,
			const std::array<std::vector<int64_t>, kBoons>& aGranted)
		{
			static const auto slots = []
			{
				std::unordered_map<int32_t, uint8_t> m;
				for (const SkillSlot& s : kSkillSlots) { m[s.Skill] = s.Slot; }
				return m;
			}();
			static const auto kinds = []
			{
				std::unordered_multimap<int32_t, std::string> m;
				for (const SkillKind& k : kSkillKinds) { m.emplace(k.Skill, k.Kind); }
				return m;
			}();
			const int64_t at = aT - aStart;
			// a cast that started up to aBefore ms before the boon (a cast time; a weapon swap is instant)
			auto castNear = [&](int64_t aBefore, auto aMatch)
			{
				for (auto& [ms, skill] : aCasts) { if (ms >= at - aBefore && ms <= at + 200 && aMatch(skill)) { return true; } }
				return false;
			};
			int32_t best = 0, interval = 0;
			int bestRank = 0, intervals = 0;
			for (const TraitBoon& tr : kTraitBoons)
			{
				if (tr.Prof != aSrc.ProfId || (tr.Spec != 0 && tr.Spec != aSrc.EliteId) || !(tr.Boons >> aBoon & 1u)) { continue; }
				int rank = 0;
				switch (tr.Trigger)
				{
				case 1: rank = std::any_of(aSrc.DodgeMs.begin(), aSrc.DodgeMs.end(), [&](int32_t d) { return d >= at - 300 && d <= at + 100; }) ? 1 : 0; break;
				case 2: rank = castNear(200, [](int32_t s) { return s == kWeaponSwap; }) ? 2 : 0; break;
				case 3: rank = castNear(1500, [](int32_t s) { auto it = slots.find(s); return it != slots.end() && it->second == 1; }) ? 3 : 0; break;
				case 4: rank = castNear(1500, [](int32_t s) { auto it = slots.find(s); return it != slots.end() && it->second == 3; }) ? 3 : 0; break;
				case 5:
					rank = castNear(1500, [&](int32_t s)
					{
						auto [a, b] = kinds.equal_range(s);
						return std::any_of(a, b, [&](const auto& k) { return k.second == tr.Arg; });
					}) ? 4 : 0;
					break;
				case 7:
					for (int b = 0; b < kBoons && !rank; b++)
					{
						if (!std::strstr(tr.Arg, kBoonNames[b])) { continue; }
						for (int64_t g : aGranted[b]) { if (std::llabs(g - aT) <= 10) { rank = 5; break; } }
					}
					break;
				case 6: intervals++; interval = tr.Trait; break;
				default: break;
				}
				if (rank > bestRank) { bestRank = rank; best = tr.Trait; }
			}
			if (best) { return kTraitBase - best; }
			return intervals == 1 ? kTraitBase - interval : 0;
		}

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

		// Skills that give a boon to the caster only: never the source of that boon on someone else
		uint32_t SelfOnly(int32_t aSkill)
		{
			switch (aSkill)
			{
			case 14412: return 1u << kStability | 1u << 9; // Balanced Stance: stability and swiftness, self (the user, 2026-09-28)
			case 14413: return 1u << kStability;           // Dolyak Signet
			default: return 0;
			}
		}

		// aToOthers: the boon went to someone other than the giver
		int32_t AttributeBoon(const std::vector<Window>& aWindows, int64_t aT, int aBoon, const Givers& aLearned, const Reach& aReach, bool aToOthers)
		{
			static const std::unordered_map<int32_t, uint32_t> api = []
			{
				std::unordered_map<int32_t, uint32_t> m(std::begin(kBoonSkills), std::end(kBoonSkills));
				// Elite Insights names some instant uses "X or Y" (a Firebrand mantra's first or second charge: one effect for
				// both) with an id of its own: it gives what either gives. Without this the mantras' quickness and might had no
				// source (the learned evidence can't see a boon the Firebrand gives near constantly): 22% of the Firebrands'
				// quickness and 33% of their might sat in "other sources" on 25 Sept.
				const std::pair<int32_t, std::array<int32_t, 2>> either[] = {
					{-20, {41475, 42960}},  // Restoring Reprieve or Rejuvenating Respite (Mantra of Solace)
					{-21, {42864, 44248}},  // Opening Passage or Clarified Conclusion (Mantra of Lore)
					{-22, {42983, 41988}},  // Potent Haste or Overwhelming Celerity (Mantra of Potence)
					{-23, {40114, 41328}},  // Portent of Freedom or Unhindered Delivery (Mantra of Liberation)
					{-29, {5966, 30564}},   // Healing Mist or Soothing Detonation
					{-55, {9161, 13684}},   // Symbol of Protection or its lesser one
					{-56, {9146, 13677}},   // Symbol of Resolution or its lesser one
				};
				for (auto& [id, parts] : either)
				{
					uint32_t bits = 0;
					for (int32_t p : parts) { if (auto it = m.find(p); it != m.end()) { bits |= it->second; } }
					if (bits) { m[id] |= bits; }
				}
				return m;
			}();
			auto gives = [&](int32_t aSkill)
			{
				if (aToOthers && (SelfOnly(aSkill) >> aBoon & 1)) { return false; }
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
			// Names that are also boons, conditions or passives: the self-applied Protection boon read as the pet skill
			// "Protection" (197 uses on 22 Sept where Elite Insights had none), the virtues' passive buffs coming back
			// at the end of the recharge (Virtue of Justice / Resolve / Courage: 75, Elite Insights none)
			static const std::unordered_set<std::string> never = {"Protection", "Fear", "Virtue of Courage", "Virtue of Justice", "Virtue of Resolve"};
			if (never.count(aName)) { return U_None; }
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
		std::unordered_map<uint64_t, std::map<uint64_t, std::pair<int, int64_t>>> healMoments; // source -> time -> (heals, amount)
		std::unordered_map<uint64_t, int64_t> healTotal;
		std::unordered_map<uint64_t, uint32_t> teamOf; // player -> WvW team id
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
			case SC_TeamChange:
				// the new team: dst when it's set, else value (where this ArcDPS build writes it; the readme says dst)
				if (uint32_t t = e.Dst ? static_cast<uint32_t>(e.Dst) : static_cast<uint32_t>(e.Value); t) { teamOf[e.Src] = t; }
				break;
			case SC_WvwTeams: f.TeamIds = {static_cast<uint32_t>(e.Dst >> 32), static_cast<uint32_t>(e.Value), static_cast<uint32_t>(e.BuffDmg)}; break;
			case SC_BuffInfo:
				if (int b = BoonIndex(e.Skill); b >= 0) { uint8_t type = e.Pad61 & 0xFF; f.Intensity[b] = type == 0 || type == 4; }
				break;
			case SC_ExtensionCombat:
				if (e.Offcycle & kHealSelfReported)
				{
					int64_t amount = e.Value < 0 ? -static_cast<int64_t>(e.Value) : e.BuffDmg < 0 ? -static_cast<int64_t>(e.BuffDmg) : 0;
					auto& m = healMoments[e.Src][e.Time];
					m.first++; m.second += amount;
					healTotal[e.Src] += amount;
				}
				break;
			default: break;
			}
		}
		// a player's WvW team as a colour (0 red, 1 blue, 2 green), -1 when the log doesn't say
		auto colourOf = [&](uint64_t aAddr) -> int
		{
			auto t = teamOf.find(aAddr);
			if (t == teamOf.end()) { return -1; }
			for (int c = 0; c < 3; c++) { if (f.TeamIds[c] && f.TeamIds[c] == t->second) { return c; } }
			return -1;
		};
		{
			// the squad's colour: most of its members'
			int n[3] = {0, 0, 0};
			for (uint64_t a : squad) { if (int c = colourOf(a); c >= 0) { n[c]++; } }
			for (int c = 0; c < 3; c++) { if (n[c] > 0 && (f.SquadTeam < 0 || n[c] > n[f.SquadTeam])) { f.SquadTeam = c; } }
		}
		// players of our own server outside the squad are nameless like enemies, but their team says they're allies: not
		// foes (2 Oct 20:18: a red "enemy" who never hit us, whose death counted as an enemy's)
		if (f.SquadTeam >= 0)
		{
			for (auto it = foes.begin(); it != foes.end();) { it = colourOf(*it) == f.SquadTeam ? foes.erase(it) : std::next(it); }
		}
		// Heals that reached the log late, in one batch: another player's Healing Stats sends their heals over the
		// network, and a backlog arrives stamped with one moment (25 Sept: a Troubadour's 159k of healing all at 1:30,
		// in a 2-minute round). The amounts are right, the moment isn't: 20+ heals at one millisecond carrying a
		// quarter or more of the player's healing count in totals but not on any time line. A batch spreads over a few
		// tens of milliseconds (37735 to 37770 ms, 232 heals), so heals less than 50 ms apart form one moment.
		std::set<std::pair<uint64_t, uint64_t>> lateHeals; // (source, time)
		for (auto& [src, moments] : healMoments)
		{
			std::vector<uint64_t> run;
			int count = 0;
			int64_t amount = 0;
			auto close = [&]
			{
				if (count >= 20 && amount * 4 >= healTotal[src]) { for (uint64_t tm : run) { lateHeals.insert({src, tm}); } }
				run.clear(); count = 0; amount = 0;
			};
			for (auto& [tm, m] : moments)
			{
				if (!run.empty() && tm - run.back() > 50) { close(); }
				run.push_back(tm); count += m.first; amount += m.second;
			}
			close();
		}
		if (strikeMin == INT64_MAX) { throw std::runtime_error("no combat in this log"); }
		if (start == INT64_MAX || end == INT64_MIN || end <= start) { start = strikeMin; end = strikeMax; }
		f.DurationMs = std::max<int64_t>(1, end - start); // one hit: 0 would divide by zero (Session skips it anyway)
		f.LogStart = start;
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
			if (added)
			{
				f.Enemies.push_back({SpecName(log.Agents.at(aAddr)), {}});
				f.Enemies.back().Team = colourOf(aAddr);
			}
			return it->second;
		};
		const size_t seconds = static_cast<size_t>(f.DurationMs / 1000 + 1);
		f.OutPerS.assign(seconds, 0);
		std::vector<int64_t> toPlayersPerS(seconds, 0); // our damage to enemy players: what our spikes are about
		f.InPerS.assign(seconds, 0);
		f.SupportPerS.assign(seconds, 0);
		f.InvulnOurs.assign(seconds, {});
		f.InvulnTheirs.assign(seconds, {});
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
		std::unordered_set<uint64_t> ownBuff;              // player ^ buff: last applied by the player themselves
		// Unblockable and Signet of Might's ready buff, per player (Player::UnblockableOn, MightSignetReady): a count of
		// stacks on, a span from the first on to the last off
		std::unordered_set<int32_t> unblockIds;
		int32_t mightSignetId = 0;
		for (auto& [id, n] : log.Skills) { if (n == "Unblockable") { unblockIds.insert(id); } else if (n == "Signet of Might") { mightSignetId = id; } }
		struct OpenSpan { int Count = 0; int64_t Since = 0; };
		std::unordered_map<uint64_t, OpenSpan> unblockOpen, signetOpen;
		auto spanOn = [&](std::unordered_map<uint64_t, OpenSpan>& aOpen, uint64_t aWho, int64_t aT)
		{
			OpenSpan& o = aOpen[aWho];
			if (o.Count++ == 0) { o.Since = aT; }
		};
		auto spanOff = [&](std::unordered_map<uint64_t, OpenSpan>& aOpen, uint64_t aWho, int64_t aT, bool aAll, std::vector<std::pair<int32_t, int32_t>> Player::*aSpans)
		{
			auto it = aOpen.find(aWho);
			if (it == aOpen.end() || it->second.Count <= 0) { return; }
			it->second.Count = aAll ? 0 : it->second.Count - 1;
			if (it->second.Count > 0) { return; }
			if (Player* p = player(aWho)) { (p->*aSpans).push_back({rel(it->second.Since), rel(aT)}); }
		};
		struct BuffUse { uint64_t Player; uint32_t Id; int64_t T; };
		std::vector<BuffUse> buffUses; // added after the events, unless the skill's own cast is there (see below)
		auto noteUse = [&](uint64_t aPlayer, uint32_t aId, int64_t aT)
		{
			uint64_t key = aPlayer * 1000003ULL ^ aId;
			auto it = lastUse.find(key);
			if (it != lastUse.end() && aT - it->second < 1000) { return; }
			lastUse[key] = aT;
			buffUses.push_back({aPlayer, aId, aT});
		};
		std::unordered_map<uint64_t, std::array<std::vector<int64_t>, kBoons>> boonApplied; // source -> boon -> times
		std::unordered_map<uint64_t, std::array<std::vector<int32_t>, kBoons>> boonDurations; // source -> boon -> durations given to others
		std::unordered_map<uint64_t, int64_t> lastStabStrip; // holder -> when the enemy last removed all their stability
		std::unordered_map<uint64_t, int64_t> lastFoeStabStrip; // enemy -> when we last removed all their stability
		// what made an ally invulnerable, for the enemy hits it absorbed: Distortion (Tale of the August Queen, a Mesmer's
		// own), or Determined and Resurrection, which come with going down and with a revive (5 Oct: 58% and 38% of the hits
		// absorbed; the user, 2026-10-06: absorbed hits outside any Tale looked like invulnerability nobody used)
		std::unordered_map<uint64_t, int64_t> distortionUntil, downInvulnUntil;
		std::unordered_map<uint32_t, bool> downInvulnId;
		auto isDownInvuln = [&](uint32_t aSkill)
		{
			auto it = downInvulnId.find(aSkill);
			if (it == downInvulnId.end()) { std::string n = log.SkillName(static_cast<int32_t>(aSkill)); it = downInvulnId.emplace(aSkill, n == "Determined" || n == "Resurrection").first; }
			return it->second;
		};
		std::unordered_map<uint64_t, std::vector<int64_t>> guardianSelfStab; // Guardian -> stability they gave themselves
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
					if (Player* p = player(e.Dst); p && !e.Buff && (e.Result == RESULT_ABSORB || e.Result == RESULT_BLOCK || e.Result == RESULT_EVADE || e.Result == RESULT_BLIND))
					{
						// an enemy strike that did nothing: what took it
						if (uint64_t o = owner(e.Src, e.SrcMaster); foes.count(o))
						{
							uint8_t kind = e.Result == RESULT_BLOCK ? 1 : e.Result == RESULT_EVADE ? 2 : e.Result == RESULT_BLIND ? 3 : 5;
							if (kind == 5 && distortionUntil[e.Dst] > t) { kind = 0; }
							else if (kind == 5 && downInvulnUntil[e.Dst] > t) { kind = 4; }
							f.NegatedHits.push_back({rel(t), static_cast<int32_t>(e.Skill), static_cast<int>(index[e.Dst]), enemy(o), kind});
						}
					}
					if (Player* p = player(e.Dst))
					{
						if (e.Result == RESULT_ABSORB)
						{
							p->Invulns++;
							if (int64_t bin = (t - start) / 1000; !e.Buff && bin >= 0 && bin < static_cast<int64_t>(seconds)) { f.InvulnOurs[bin].push_back(index[e.Dst]); }
						}
						else if (!e.Buff && e.Result == RESULT_EVADE)
						{
							p->Evades++;
							if (uint64_t o = owner(e.Src, e.SrcMaster); foes.count(o)) { p->EvadedIn.push_back({rel(t), static_cast<int32_t>(e.Skill), 0, enemy(o)}); }
						}
						else if (!e.Buff && e.Result == RESULT_BLOCK) { p->Blocks++; }
					}
				}
				// our hits an enemy's invulnerability absorbed (their distortion, Tale of the August Queen): when they were
				if (e.Result == RESULT_ABSORB && !e.Buff && foes.count(e.Dst))
				{
					if (int64_t bin = (t - start) / 1000; bin >= 0 && bin < static_cast<int64_t>(seconds)) { if (int en = enemy(e.Dst); en >= 0) { f.InvulnTheirs[bin].push_back(en); } }
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
						p->HitsOut.push_back({rel(t), static_cast<int32_t>(e.Skill), static_cast<int32_t>(dmg), enemy(e.Dst), e.Buff ? 0 : static_cast<int32_t>(std::min<int64_t>(e.Overstack, dmg))});
						f.SquadDamage += dmg; // to enemy players, as EnemyDamage is to ours (pets: SquadPetsTook, EnemyPetsTook)
					}
					if (inWindow) { f.OutPerS[bin] += dmg; p->DamagePerS[bin] += static_cast<int32_t>(dmg); }
				}
				else if (Player* hit = player(e.Dst); hit && !friends.count(o))
				{
					hit->DamageTaken += dmg;
					if (!e.Buff) { f.StrikesIn.push_back({rel(t), static_cast<int>(index[e.Dst])}); }
					hit->HitsIn.push_back({rel(t), static_cast<int32_t>(e.Skill), static_cast<int32_t>(dmg), enemy(o), e.Buff ? 0 : static_cast<int32_t>(std::min<int64_t>(e.Overstack, dmg))});
					f.GroupDamageTaken[hit->Subgroup] += dmg;
					f.EnemyDamage += dmg;
					if (inWindow) { f.InPerS[bin] += dmg; }
				}
				// Pets and minions (ranger pets, necromancer minions, clones, turrets) taking hits that could have hit a player
				if (uint64_t d = owner(e.Dst, e.DstMaster); d && d != e.Dst)
				{
					if (Player* keeper = player(d); keeper && foes.count(o)) { keeper->PetsTook += dmg; f.SquadPetsTook += dmg; }
					else if (foes.count(d) && player(o)) { f.EnemyPetsTook += dmg; }
				}
				// the enemy squad: enemies who hit us or took our hits, through their minions too (not ones only fighting a
				// third side; an enemy healer who never hit us but took our damage counts: 13 -> 25 on 28 Sept, 20:13)
				if (foes.count(o) && friends.count(e.Dst)) { enemiesSeen.insert(o); f.Enemies[enemy(o)].Fought = true; }
				if (player(o) && foes.count(e.Dst)) { enemiesSeen.insert(e.Dst); f.Enemies[enemy(e.Dst)].Fought = true; }
				break;
			}
			case SC_ExtensionCombat:
			{
				if (!(e.Offcycle & kHealSelfReported)) { break; }
				int64_t amount = e.Value < 0 ? -static_cast<int64_t>(e.Value) : e.BuffDmg < 0 ? -static_cast<int64_t>(e.BuffDmg) : 0;
				Player* p = player(owner(e.Src, e.SrcMaster));
				if (!amount || !p || !friends.count(e.Dst)) { break; }
				p->HealKnown = true;
				const bool late = lateHeals.count({e.Src, e.Time}) > 0; // counts, but its moment isn't known
				if (!(e.Offcycle & kHealDowned)) { p->Skills[static_cast<int32_t>(e.Skill)].Hits++; }
				if (e.Value < 0 && !late) { heals[p->Addr].push_back({t, e.Dst, static_cast<int32_t>(e.Skill)}); } // direct heals only
				if (int64_t bin = (t - start) / 1000; !late && !(e.Offcycle & kHealDowned) && bin >= 0 && bin < static_cast<int64_t>(seconds)) { f.SupportPerS[bin] += amount; }
				if (e.Shields) { p->Barrier += amount; p->Skills[static_cast<int32_t>(e.Skill)].Barrier += amount; }
				else if (e.Offcycle & kHealDowned)
				{
					p->HealDowned += amount;
					if (Player* to = player(e.Dst)) { to->DownHealsIn.push_back({rel(t), static_cast<int32_t>(e.Skill), index[p->Addr], static_cast<int32_t>(amount)}); }
				}
				else
				{
					p->Heal += amount;
					p->Skills[static_cast<int32_t>(e.Skill)].Heal += amount;
					if (late) { p->HealLate += amount; if (p->HealLateMs < 0) { p->HealLateMs = rel(t); } }
					if (Player* to = player(e.Dst); to && !late) { to->HealsIn.push_back({rel(t), static_cast<int32_t>(amount)}); }
					if (int64_t bin = (t - start) / 1000; !late && bin >= 0 && bin < static_cast<int64_t>(seconds)) { p->HealPerS[bin] += static_cast<int32_t>(amount); }
					const Agent* target = log.Find(e.Dst);
					if (e.Dst == p->Addr) { p->HealToSelf += amount; }
					else if (target && target->Subgroup == p->Subgroup) { p->HealToGroup += amount; }
					else { p->HealToOthers += amount; }
				}
				break;
			}
			case SC_BuffRemoveAll:
			{
				if (e.Skill == 10243) { distortionUntil[e.Src] = std::min(distortionUntil[e.Src], t); }
				else if (isDownInvuln(e.Skill)) { downInvulnUntil[e.Src] = std::min(downInvulnUntil[e.Src], t); }
				if (e.Skill == kBoonIds[kStability] && foes.count(e.Src)) { lastFoeStabStrip[e.Src] = t; }
				if (unblockIds.count(static_cast<int32_t>(e.Skill))) { spanOff(unblockOpen, e.Src, t, true, &Player::UnblockableOn); }
				if (mightSignetId && static_cast<int32_t>(e.Skill) == mightSignetId) { spanOff(signetOpen, e.Src, t, true, &Player::MightSignetReady); }
				// Illusion of Life ending: ran out (at its full length) or ended early
				if (Player* holder = player(e.Src); holder && isIllusion(e.Skill) && !holder->IllusionOfLife.empty())
				{
					auto& il = holder->IllusionOfLife.back();
					int32_t length = illusionLength[e.Src];
					il.RanOut = length > 0 && rel(t) - il.From >= length - 200; // length unknown: not claimed
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
				// (their own passive only: a Necromancer's Signet of Vampirism mark on a Guardian coming off isn't a use)
				if (player(e.Src) && !cannotActSince.count(e.Src) && ownBuff.count(e.Src * 1000003ULL ^ e.Skill) && useKind(e.Skill) == U_Signet) { signetOff[e.Src * 1000003ULL ^ e.Skill] = {e.Src, e.Skill, t}; }
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
				if (unblockIds.count(static_cast<int32_t>(e.Skill)) && player(e.Dst)) { spanOn(unblockOpen, e.Dst, t); }
				if (mightSignetId && static_cast<int32_t>(e.Skill) == mightSignetId && player(e.Dst)) { spanOn(signetOpen, e.Dst, t); }
				// who put each buff on a player last: themselves (a signet's passive) or someone else
				if (player(e.Dst)) { uint64_t k = e.Dst * 1000003ULL ^ e.Skill; if (e.Src == e.Dst) { ownBuff.insert(k); } else { ownBuff.erase(k); } }
				// a profession mechanic switched on: its buff of the same name on the player
				if (e.StateChange == SC_BuffApply && e.Src == e.Dst && player(e.Src) && BoonIndex(e.Skill) < 0 && !kConditions.count(e.Skill) && useKind(e.Skill) == U_Mode) { noteUse(e.Src, e.Skill, t); }
				if (e.Src == e.Dst)
				{
					auto it = legendOf.find(e.Skill);
					if (it == legendOf.end()) { it = legendOf.emplace(e.Skill, LegendOf(log.SkillName(e.Skill))).first; }
					if (Player* p = it->second ? player(e.Dst) : nullptr) { p->Legends |= it->second; }
				}
				// invulnerability on one of ours, and who gave it: Distortion (a Mesmer's own, a Troubadour's Tale of the August
				// Queen on the group). 30 Sept: on in 7 of 10 moments a hit on one of ours was absorbed. (Determined, 1 s,
				// comes only with going down.)
				if (e.StateChange == SC_BuffApply && e.Value > 0 && friends.count(e.Dst))
				{
					if (e.Skill == 10243) { distortionUntil[e.Dst] = std::max(distortionUntil[e.Dst], t + e.Value); }
					else if (isDownInvuln(e.Skill)) { downInvulnUntil[e.Dst] = std::max(downInvulnUntil[e.Dst], t + e.Value); }
				}
				if (e.StateChange == SC_BuffApply && e.Skill == 10243 && e.Value > 0)
				{
					auto to = index.find(e.Dst), by = index.find(e.Src);
					if (to != index.end()) { f.InvulnGives.push_back({rel(t), e.Value, by != index.end() ? by->second : -1, to->second, static_cast<int32_t>(e.Skill)}); }
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
					if (b == kStability && e.Src == e.Dst && player(e.Src)->ProfId == 1) { guardianSelfStab[e.Src].push_back(t); }
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
				if (unblockIds.count(static_cast<int32_t>(e.Skill))) { spanOff(unblockOpen, e.Src, t, false, &Player::UnblockableOn); }
				if (mightSignetId && static_cast<int32_t>(e.Skill) == mightSignetId) { spanOff(signetOpen, e.Src, t, false, &Player::MightSignetReady); }
				uint64_t key = stackKey(e.Src, e.Pad61);
				// an enemy's stability stack one of ours took on its own, not in a strip: our CC that it blocked
				if (e.Skill == kBoonIds[kStability] && foes.count(e.Src))
				{
					auto ls = lastFoeStabStrip.find(e.Src);
					if (Player* by = player(owner(e.Dst, 0)); by && (ls == lastFoeStabStrip.end() || t - ls->second > 5)) { by->CcIntoStab++; f.EnemyStabUsedUp++; }
				}
				if (auto it = stabIndex.find(key); it != stabIndex.end() && e.Skill == kBoonIds[kStability])
				{
					stab[it->second].Removed = std::min(stab[it->second].Removed, t);
					// taken by the enemy: part of a strip (right after a remove-all), else used up one at a time
					if (e.Iff == IFF_FOE)
					{
						stab[it->second].ByFoe = t;
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
		// Down contribution, Elite Insights' rule (OffensiveStatistics, SingleActorStatusHelper.IsDownBeforeNext90, read
		// 2026-10-02; its docs still say "a down that led to a death", the code doesn't): health damage (not into
		// barrier) to an enemy at 90% health or less, not downed, who goes down before their health is next above 90%
		{
			for (Player& p : f.Players)
			{
				for (const auto& h : p.HitsOut)
				{
					if (h.Enemy < 0) { continue; }
					const auto& en = f.Enemies[h.Enemy];
					// health now: the last update at or before the hit (none: unknown, not counted)
					auto at = std::upper_bound(en.Hp.begin(), en.Hp.end(), h.Ms, [](int32_t ms, const std::pair<int32_t, int32_t>& u) { return ms < u.first; });
					if (at == en.Hp.begin() || std::prev(at)->second > 9000) { continue; }
					bool downed = false;
					int32_t nextDown = INT32_MAX;
					for (const Span& sp : en.DownSpans)
					{
						if (sp.Dead) { continue; }
						if (sp.From <= h.Ms && sp.To > h.Ms) { downed = true; break; }
						if (sp.From >= h.Ms) { nextDown = std::min(nextDown, sp.From); }
					}
					if (downed || nextDown == INT32_MAX) { continue; }
					auto above = std::find_if(at, en.Hp.end(), [&](const std::pair<int32_t, int32_t>& u) { return u.first > h.Ms && u.second > 9000; });
					if (above != en.Hp.end() && above->first < nextDown) { continue; }
					p.DownContribution += h.Damage;
				}
			}
		}
		for (auto& [key, off] : signetOff) { if (end - off.Time >= 2000) { noteUse(off.Player, off.Buff, off.Time); } }
		for (auto& [who, o] : unblockOpen) { if (o.Count > 0) { if (Player* p = player(who)) { p->UnblockableOn.push_back({rel(o.Since), rel(end)}); } } }
		for (auto& [who, o] : signetOpen) { if (o.Count > 0) { if (Player* p = player(who)) { p->MightSignetReady.push_back({rel(o.Since), rel(end)}); } } }
		// A use seen from a buff or a signet's passive counts unless the player cast a skill of that name within 1.5 s:
		// then it's that cast, already counted (the buff has an id of its own, so it would be counted twice)
		{
			std::unordered_map<int32_t, std::string> nameOf;
			auto name = [&](int32_t aId) -> const std::string& { auto it = nameOf.find(aId); if (it == nameOf.end()) { it = nameOf.emplace(aId, log.SkillName(static_cast<uint32_t>(aId))).first; } return it->second; };
			for (const BuffUse& u : buffUses)
			{
				const std::string& n = name(static_cast<int32_t>(u.Id));
				bool cast = false;
				for (auto& [tm, skill] : casts[u.Player]) { if (std::llabs(tm - (u.T - start)) <= 1500 && !n.empty() && name(skill) == n) { cast = true; break; } }
				if (cast) { continue; }
				casts[u.Player].push_back({u.T - start, static_cast<int32_t>(u.Id)});
				windows[u.Player].push_back({u.T - kInstantSlack, u.T + kInstantSlack, static_cast<int32_t>(u.Id), true});
			}
		}
		// "Stand Your Ground!" (instant, no cast event; Elite Insights' rule is custom code, so not in CastRules.inc): a
		// Guardian giving themselves 5+ stability stacks at one moment, as Elite Insights tells it apart (it also needs
		// the shout's effect, which the stack count stands in for here)
		for (auto& [g, times] : guardianSelfStab)
		{
			std::sort(times.begin(), times.end());
			for (size_t i = 0; i < times.size();)
			{
				size_t j = i;
				while (j < times.size() && times[j] - times[i] <= 50) { j++; }
				const int64_t at = times[i];
				const size_t n = j - i;
				i = j;
				bool logged = std::any_of(casts[g].begin(), casts[g].end(), [&](const auto& c) { return c.second == 9153 && std::llabs(c.first - (at - start)) <= 1000; });
				if (n < 5 || logged) { continue; }
				casts[g].push_back({at - start, 9153});
				windows[g].push_back({at - kInstantSlack, at + kInstantSlack, 9153, true});
			}
		}
		// Instant casts from Elite Insights' rules, unless the same skill's cast is in the log within 1 s (a rule for a
		// skill that also has an animation would count it twice)
		for (auto& [caster, found] : InstantCasts::Find(log))
		{
			if (!player(caster)) { continue; }
			auto& mine = casts[caster];
			const size_t logged = mine.size();
			for (auto& [t, skill] : found)
			{
				bool dup = false;
				for (size_t i = 0; i < logged; i++) { if (mine[i].second == skill && std::llabs(mine[i].first - (t - start)) <= 1000) { dup = true; break; } }
				if (dup) { continue; }
				mine.push_back({t - start, skill});
				windows[caster].push_back({t - kInstantSlack, t + kInstantSlack, skill, true});
			}
		}
		for (auto& [addr, since] : deadSince) { deadMs[addr] += end - since; }
		f.EnemyCount = static_cast<int>(enemiesSeen.size());

		// Spikes and timing
		// Our spikes from damage to enemy players only (not NPCs, siege or gates). Runs of seconds (Spikes): on 110 rounds of
		// 30 s+ 63% of our spikes had an enemy down from 1 s before their start to 4 s after their end, and 79% of enemy downs
		// fell in such a window; enemy spikes 59% and 85%. (Peaks 3 s apart, 2026-09-24 to 10-06: 63% / 68%, 59% / 73%.)
		for (const SpikeRun& r : Spikes(toPlayersPerS)) { f.OurSpikesMs.push_back(r.Peak); f.OurSpikeSpans.push_back({r.From, r.To}); }
		f.ToPlayersPerS = std::move(toPlayersPerS);
		for (const SpikeRun& r : Spikes(f.InPerS)) { f.TheirSpikesMs.push_back(r.Peak); f.TheirSpikeSpans.push_back({r.From, r.To}); }
		for (size_t s = 0; s < seconds; s++)
		{
			auto c = Classify(static_cast<int64_t>(s) * 1000 + 500, f);
			for (int k = 0; k < T_Count; k++) { f.TimingBaseline[k] += c[k] ? 1.0 / seconds : 0.0; }
		}

		for (Player& p : f.Players)
		{
			p.ActiveMs = std::max<int64_t>(0, f.DurationMs - deadMs[p.Addr]);
			// A skill the log records as two ids of one name, one right after the other (Paragon's Line Breaker: the
			// charge and the landing, 305 of each on 22 Sept; Rend): one use. Elite Insights counts one.
			{
				auto& list = casts[p.Addr];
				std::sort(list.begin(), list.end());
				std::vector<std::pair<int64_t, int32_t>> kept;
				for (const auto& c : list)
				{
					bool part = false;
					for (auto it = kept.rbegin(); it != kept.rend() && c.first - it->first <= 1500; ++it)
					{
						if (it->second != c.second && c.second > 0 && it->second > 0 && log.SkillName(static_cast<uint32_t>(it->second)) == log.SkillName(static_cast<uint32_t>(c.second)) &&
							!log.SkillName(static_cast<uint32_t>(c.second)).empty()) { part = true; break; }
					}
					if (!part) { kept.push_back(c); }
				}
				list.swap(kept);
			}
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
		std::map<std::pair<Player*, int32_t>, std::vector<int64_t>> durationUses; // (giver, skill) -> moments
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
			if (skill) { durationUses[{src, skill}].push_back(c.Applied - start); }
			if (!skill) { skill = AttributeBoon(windows[src->Addr], c.Applied, c.Boon, learned[src->Addr], reach[src->ProfId], src != tgt); }
			if (!skill && src != tgt) { skill = TraitFor(*src, c.Applied, start, c.Boon, casts[src->Addr], boonApplied[src->Addr]); }
			// stability given: not a self-only skill's (Balanced Stance never reaches anyone else, so "it missed them"
			// would be wrong; the user, 2026-09-28)
			if (c.Boon == kStability && !(SelfOnly(skill) >> kStability & 1)) { gives.push_back({src, rel(c.Applied), skill, static_cast<int>(tgt - f.Players.data())}); }
			if (src == tgt) { continue; }
			if (skill == 0 && (src->OtherBoonMs.empty() || src->OtherBoonMs.back() != std::make_pair(rel(c.Applied), c.Boon))) { src->OtherBoonMs.push_back({rel(c.Applied), c.Boon}); }
			double s = c.Ms / 1000.0;
			src->BoonSquadS[c.Boon] += s;
			src->Skills[skill].BoonSquadS[c.Boon] += s;
			if (src->Subgroup == tgt->Subgroup)
			{
				src->BoonGroupS[c.Boon] += s;
				src->Skills[skill].BoonGroupS[c.Boon] += s;
			}
		}
		for (auto& [key, moments] : durationUses)
		{
			auto [p, skill] = key;
			SkillRow& row = p->Skills[skill];
			std::sort(moments.begin(), moments.end());
			int64_t prev = -1000000; // before any moment (INT64_MIN would overflow the difference below)
			int64_t useStart = -1000000;
			for (int64_t m : moments)
			{
				// one use: the stacks to every ally at that moment, and a field's pulses after it, each within 1.5 s of
				// the one before (Hallowed Ground pulses once a second for about 8 s: each pulse was a "use", 5 in a
				// 28 s round)
				bool chained = m - prev <= 1500;
				prev = m;
				if (chained) { continue; }
				useStart = m;
				bool cast = std::any_of(row.CastMs.begin(), row.CastMs.end(), [&](int32_t c) { return std::llabs(c - useStart) <= 1000; });
				if (cast) { continue; }
				row.Casts++;
				row.CastMs.push_back(static_cast<int32_t>(m));
				auto cl = Classify(m, f);
				for (int k = 0; k < T_Count; k++) { row.Timing[k] += cl[k]; }
			}
			std::sort(row.CastMs.begin(), row.CastMs.end());
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
		// Stability a skill gives the caster only: under one moment in ten this round reached anyone else (30 Sept and
		// 25 Sept: a Chronomancer's shatters 1%, Distortion 0%, a Troubadour's Harp, Lute, Flute and Drum Playing 0%,
		// Lively Lute 6%; Power Break 92% to 98%). Deaths said "it missed" a teammate for them (the user, 2026-09-30).
		for (Player& p : f.Players)
		{
			const int self = static_cast<int>(&p - f.Players.data());
			std::map<int32_t, std::pair<int, int>> reachOthers; // skill -> (moments, reaching someone else)
			for (const auto& g : p.StabGives)
			{
				auto& r = reachOthers[g.Skill];
				r.first++;
				r.second += std::any_of(g.Targets.begin(), g.Targets.end(), [&](int t) { return t != self; });
			}
			for (auto& g : p.StabGives) { auto& r = reachOthers[g.Skill]; g.SelfOnly = g.Skill != 0 && r.second * 10 < r.first; }
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
		auto canAct = [&](uint64_t aPlayer, int64_t a, int64_t b)
		{
			for (auto& [from, to] : cannotAct[aPlayer]) { if (from <= a && to >= b) { return false; } }
			return true;
		};
		// Redundancy, TopStats' definition (reference/topstats_stability_guide.txt): a stack given to an ally who
		// already had another provider's stability is charged its overlap with it, to the later provider, unless at
		// most one of those earlier stacks would still be on 3 s later. Self-given stability is left out on both
		// sides, a provider's own repeats are never charged, applications in the same ms are left unassigned. Each
		// stack runs its nominal length, cut short only when the enemy took it (a strip or a CC using it up).
		{
			auto nominalStop = [&](const StabStack* x) { return std::min({x->Removed, x->NominalEnd, end}); };
			// presence and charges as time per provider and ally, overlaps merged: one cast puts several stacks on an
			// ally at once (summed, our presence came out 3x TopStats' on 22 Sept)
			auto merged = [](std::vector<std::pair<int64_t, int64_t>>& v)
			{
				std::sort(v.begin(), v.end());
				int64_t total = 0, a = INT64_MIN, b = INT64_MIN;
				for (auto& [x, y] : v)
				{
					if (x > b) { total += b > a ? b - a : 0; a = x; b = y; }
					else { b = std::max(b, y); }
				}
				return total + (b > a ? b - a : 0);
			};
			for (auto& [target, list] : stabOn)
			{
				std::vector<const StabStack*> given;
				for (const StabStack* x : list) { if (x->Source != target && player(x->Source)) { given.push_back(x); } }
				std::sort(given.begin(), given.end(), [](const StabStack* l, const StabStack* r) { return l->Applied < r->Applied; });
				std::vector<const StabStack*> live; // applied earlier, still on
				std::map<uint64_t, std::pair<std::vector<std::pair<int64_t, int64_t>>, std::vector<std::pair<int64_t, int64_t>>>> spans; // provider -> (on, charged)
				for (const StabStack* x : given)
				{
					live.erase(std::remove_if(live.begin(), live.end(), [&](const StabStack* o) { return nominalStop(o) <= x->Applied; }), live.end());
					int64_t from = std::max(x->Applied, start), to = nominalStop(x);
					auto& [on, charged] = spans[x->Source];
					if (to > from) { on.push_back({from, to}); }
					int64_t latest = INT64_MIN;
					int later = 0;
					for (const StabStack* o : live)
					{
						if (o->Applied == x->Applied) { continue; }
						later += o->NominalEnd > x->Applied + 3000;
						if (o->Source != x->Source) { latest = std::max(latest, nominalStop(o)); }
					}
					if (latest != INT64_MIN && later > 1 && std::min(to, latest) > from) { charged.push_back({from, std::min(to, latest)}); }
					live.push_back(x);
				}
				for (auto& [source, sc] : spans)
				{
					Player* src = player(source);
					src->StabAllyNominalMs += merged(sc.first);
					src->StabRedundantMs += merged(sc.second);
				}
			}
		}
		// Your stability on your subgroup in each enemy spike (the user, 2026-10-01: what counts is whether it was on
		// them when it was needed): the share of the subgroup, alive and up at the peak, that carried a stack from you
		// at some point in the 3 s up to the peak, when their CC lands (at the peak itself: 0% for most givers)
		for (Player& p : f.Players)
		{
			if (p.StabAllyMs <= 0) { continue; }
			for (int64_t peak : f.TheirSpikesMs)
			{
				const int64_t at = start + peak;
				if (!canAct(p.Addr, at, at)) { continue; }
				int members = 0, carrying = 0;
				for (const Player& m : f.Players)
				{
					if (&m == &p || m.Subgroup != p.Subgroup || !canAct(m.Addr, at, at)) { continue; }
					members++;
					const auto& on = stabOn[m.Addr];
					carrying += std::any_of(on.begin(), on.end(), [&](const StabStack* x) { return x->Source == p.Addr && x->Applied <= at && x->Removed > at - 3000; });
				}
				if (members == 0) { continue; }
				p.StabSpikes++;
				p.StabSpikeShare += double(carrying) / members;
			}
		}
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

		// Distortion used by each of ours, and how many of those uses fell in an enemy spike (3 s before its peak to 4 s
		// after, as downs count). A use: their gives within 0.3 s. It counts when it reached someone else (Tale of the
		// August Queen) or lasted 2 s or more (a Mesmer's Distortion); the 1 s self procs of traits don't.
		{
			struct Use { int32_t Ms; int By; bool Others; int32_t Duration; };
			std::vector<Use> uses;
			for (const auto& g : f.InvulnGives)
			{
				if (g.By < 0) { continue; }
				auto it = std::find_if(uses.begin(), uses.end(), [&](const Use& u) { return u.By == g.By && std::abs(u.Ms - g.Ms) <= 300; });
				if (it == uses.end()) { uses.push_back({g.Ms, g.By, false, 0}); it = uses.end() - 1; }
				it->Others |= g.To != g.By;
				it->Duration = std::max(it->Duration, g.Duration);
			}
			for (const Use& u : uses)
			{
				if (!u.Others && u.Duration < 2000) { continue; }
				Player& p = f.Players[u.By];
				p.DistortionUses++;
				bool in = false;
				for (size_t i = 0; i < f.TheirSpikesMs.size(); i++)
				{
					auto [first, last] = RunOf(f.TheirSpikesMs, f.TheirSpikeSpans, i);
					in |= u.Ms >= first - 3000 && u.Ms <= last + 4000;
				}
				p.DistortionInSpikes += in;
			}
		}
		for (auto* perS : {&f.InvulnOurs, &f.InvulnTheirs})
		{
			for (auto& who : *perS) { std::sort(who.begin(), who.end()); who.erase(std::unique(who.begin(), who.end()), who.end()); }
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
					// Spirit of Nature revives with its spirit's Nature's Renewal, 0.5 to 1.1 s after the cast (30 Sept: an
					// ally downed 0.2 s after it got up by it, and the table said "nobody down near")
					const int32_t lag = u.Skill == 12569 ? 1500 : 0;
					for (size_t i = 0; i < q.DownSpans.size(); i++)
					{
						const Span& s = q.DownSpans[i];
						if (s.Dead || s.From > u.Ms + lag || s.To < u.Ms) { continue; }
						u.DownNear++;
						bool died = i + 1 < q.DownSpans.size() && q.DownSpans[i + 1].Dead && q.DownSpans[i + 1].From == s.To;
						if (u.Skill == 10244)
						{
							const int caster = static_cast<int>(&p - f.Players.data());
							if (std::any_of(q.IllusionOfLife.begin(), q.IllusionOfLife.end(), [&](const Player::Illusion& il) { return il.By == caster && il.From >= u.Ms - 200 && il.From <= u.Ms + 2000; })) { u.GotUp++; }
						}
						else if (!died && s.To <= u.Ms + 3000) { u.GotUp++; }
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
			for (const auto& h : p.DownHealsIn)
			{
				if (f.SkillNames.count(h.Skill)) { continue; }
				std::string name = log.SkillName(static_cast<uint32_t>(h.Skill));
				f.SkillNames[h.Skill] = name.empty() ? std::to_string(h.Skill) : name;
			}
			for (auto& [skill, row] : p.Skills)
			{
				if (f.SkillNames.count(skill)) { continue; }
				std::string name;
				for (const DurationSource& d : kDurationSources) { if (d.Skill == skill) { name = d.Name; } }
				if (!name.empty()) { f.SkillNames[skill] = name; continue; }
				if (skill <= kTraitBase)
				{
					for (const TraitBoon& tr : kTraitBoons) { if (kTraitBase - tr.Trait == skill) { name = std::string("Trait: ") + tr.Name; break; } }
					f.SkillNames[skill] = name.empty() ? "a trait" : name;
					continue;
				}
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
		// One row per skill name within a profession: a trait can change a skill's id (a Troubadour's Harmonious Harp is
		// 76960 or 77077, Crescendo too), so two players on one build looked like they used different skills. The lowest
		// id of the name among that profession's rows keeps the row (not across professions: a sigil's Flame Blast
		// isn't an Elementalist's).
		{
			std::map<std::pair<uint32_t, std::string>, int32_t> first; // (profession, name) -> id
			for (const Player& p : f.Players)
			{
				for (auto& [id, row] : p.Skills)
				{
					auto n = f.SkillNames.find(id);
					if (id <= 0 || n == f.SkillNames.end() || n->second.empty() || std::to_string(id) == n->second) { continue; }
					auto it = first.emplace(std::make_pair(p.ProfId, n->second), id).first;
					it->second = std::min(it->second, id);
				}
			}
			auto keep = [&](uint32_t aProf, int32_t aSkill)
			{
				auto n = f.SkillNames.find(aSkill);
				if (aSkill <= 0 || n == f.SkillNames.end()) { return aSkill; }
				auto it = first.find({aProf, n->second});
				return it == first.end() ? aSkill : it->second;
			};
			for (Player& p : f.Players)
			{
				std::vector<int32_t> moved;
				for (auto& [id, row] : p.Skills) { if (keep(p.ProfId, id) != id) { moved.push_back(id); } }
				for (int32_t id : moved)
				{
					SkillRow from = std::move(p.Skills[id]);
					p.Skills.erase(id);
					SkillRow& to = p.Skills[keep(p.ProfId, id)];
					to.Casts += from.Casts; to.Hits += from.Hits; to.Heal += from.Heal; to.Barrier += from.Barrier;
					to.Damage += from.Damage; to.DamageAll += from.DamageAll; to.Strips += from.Strips; to.Cleanses += from.Cleanses;
					to.Interrupted += from.Interrupted; to.Cancelled += from.Cancelled;
					for (int b = 0; b < kBoons; b++) { to.BoonSquadS[b] += from.BoonSquadS[b]; to.BoonGroupS[b] += from.BoonGroupS[b]; }
					for (int k = 0; k < T_Count; k++) { to.Timing[k] += from.Timing[k]; }
					to.CastMs.insert(to.CastMs.end(), from.CastMs.begin(), from.CastMs.end());
					std::sort(to.CastMs.begin(), to.CastMs.end());
				}
				for (auto& g : p.StabGives) { g.Skill = keep(p.ProfId, g.Skill); }
				// their hits too: the spike breakdown's skill list and lines go by them (an Evoker's Fulgor is cast as 73091 and
				// hits as 73125: two "Fulgor" to pick, one with no casts on the time line; the user, 2026-09-30)
				for (auto& h : p.HitsOut) { h.Skill = keep(p.ProfId, h.Skill); }
			}
		}
		return f;
	}
}
