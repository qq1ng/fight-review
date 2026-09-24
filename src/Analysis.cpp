#include "Analysis.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "InstantCasts.h"

namespace Analysis
{
	const std::array<uint32_t, kBoons> kBoonIds = {740, 725, 1187, 30328, 717, 718, 726, 743, 1122, 719, 26980, 873};
	const std::array<const char*, kBoons> kBoonNames = {"Might", "Fury", "Quickness", "Alacrity", "Protection",
		"Regeneration", "Vigor", "Aegis", "Stability", "Swiftness", "Resistance", "Resolution"};
	const std::array<const char*, T_Count> kTimingNames = {"into ours", "ahead of theirs", "answering theirs"};

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
		constexpr int64_t kSpikeRadiusS = 3;
		constexpr double kSpikeOverMedian = 1.6, kSpikeOfMax = 0.35;
		constexpr int64_t kIntoOurs = 2000, kAheadOfTheirs = 4000, kAnsweringTheirs = 3000;

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

		struct Stack { uint64_t Source; int Boon; int64_t Since; int64_t Applied; bool Counting; };

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

		// Which boons a skill gives: the GW2 API's facts, plus what this round's log shows for the profession
		// (key: profession << 32 | skill). Boons of anything else go to 0 (a trait, relic, sigil, rune or combo).
		using Givers = std::unordered_map<uint64_t, uint32_t>;
		uint64_t GiverKey(uint32_t aProf, int32_t aSkill) { return static_cast<uint64_t>(aProf) << 32 | static_cast<uint32_t>(aSkill); }

		int32_t AttributeBoon(const std::vector<Window>& aWindows, int64_t aT, int aBoon, uint32_t aProf, const Givers& aLearned)
		{
			static const std::unordered_map<int32_t, uint32_t> api(std::begin(kBoonSkills), std::end(kBoonSkills));
			auto gives = [&](int32_t aSkill)
			{
				auto it = api.find(aSkill);
				if (it != api.end() && (it->second >> aBoon & 1)) { return true; }
				auto lt = aLearned.find(GiverKey(aProf, aSkill));
				return lt != aLearned.end() && (lt->second >> aBoon & 1) != 0;
			};
			for (int32_t skill : Candidates(aWindows, aT)) { if (gives(skill)) { return skill; } }
			// Nothing at that moment: a use that ended shortly before (a boon granted a moment after the cast)
			for (int64_t back = 250; back <= kBoonDelay; back += 250)
			{
				for (int32_t skill : Candidates(aWindows, aT - back)) { if (gives(skill)) { return skill; } }
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

	Fight Analyse(const std::filesystem::path& aPath)
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

		std::unordered_map<uint64_t, int> index;
		for (uint64_t addr : present)
		{
			const Agent& a = log.Agents[addr];
			Player p;
			p.Addr = addr;
			p.Name = a.Name;
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
		const size_t seconds = static_cast<size_t>(f.DurationMs / 1000 + 1);
		f.OutPerS.assign(seconds, 0);
		f.InPerS.assign(seconds, 0);
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
		auto rel = [&](int64_t aT) { return static_cast<int32_t>(std::clamp<int64_t>(aT - start, 0, end - start)); };
		for (Player& p : f.Players) { p.DamagePerS.assign(seconds, 0); p.HealPerS.assign(seconds, 0); }
		// Uses without a cast event (mode buffs, signets), and when each player applied each boon (evidence)
		std::unordered_map<uint32_t, UseBuff> useKinds;
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
		// Boons
		std::unordered_map<uint64_t, Stack> stacks; // key: target ^ (stack id << 1) collisions avoided below
		struct Credit { uint64_t Source, Target; int Boon; int64_t Applied, Ms; };
		std::vector<Credit> credits;
		auto stackKey = [](uint64_t aTarget, uint32_t aId) { return aTarget * 1000003ULL ^ aId; };
		std::unordered_map<uint64_t, uint64_t> stackTarget; // key -> target, to detect collisions
		auto stop = [&](uint64_t aKey, uint64_t aTarget, int64_t aT)
		{
			auto it = stacks.find(aKey);
			if (it == stacks.end() || !it->second.Counting) { return; }
			int64_t t0 = std::max(it->second.Since, start), t1 = std::min(aT, end);
			if (t1 > t0) { credits.push_back({it->second.Source, aTarget, it->second.Boon, it->second.Applied, t1 - t0}); }
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
						else if (!e.Buff && e.Result == RESULT_EVADE) { p->Evades++; }
						else if (!e.Buff && e.Result == RESULT_BLOCK) { p->Blocks++; }
					}
				}
				if (e.Result == kResultCrowdControl && !e.Buff)
				{
					if (Player* p = player(e.Dst)) { p->CcTaken++; p->CcTakenMs += std::max(0, e.Value); ccContacts[e.Dst].push_back(t); p->CcMs.push_back(rel(t)); }
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
					if (foes.count(e.Dst)) { p->Damage += dmg; p->Skills[static_cast<int32_t>(e.Skill)].Damage += dmg; }
					f.SquadDamage += dmg;
					if (inWindow) { f.OutPerS[bin] += dmg; p->DamagePerS[bin] += static_cast<int32_t>(dmg); }
				}
				else if (Player* hit = player(e.Dst); hit && !friends.count(o))
				{
					hit->DamageTaken += dmg;
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
				if (e.Value < 0) { heals[p->Addr].push_back({t, e.Dst, static_cast<int32_t>(e.Skill)}); } // direct heals only
				if (e.Shields) { p->Barrier += amount; p->Skills[static_cast<int32_t>(e.Skill)].Barrier += amount; }
				else if (e.Offcycle & kHealDowned) { p->HealDowned += amount; }
				else
				{
					p->Heal += amount;
					p->Skills[static_cast<int32_t>(e.Skill)].Heal += amount;
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
				// src lost the buff: a signet's passive coming off is the signet being used (not while down or dead)
				// (a use only if it stays off 2 s: the recharge; some signets churn their buff many times a second)
				if (player(e.Src) && !cannotActSince.count(e.Src) && useKind(e.Skill) == U_Signet) { signetOff[e.Src * 1000003ULL ^ e.Skill] = {e.Src, e.Skill, t}; }
				Player* p = player(e.Dst); // dst removed it; its own removals only, not a pet's
				if (!p) { break; }
				if (BoonIndex(e.Skill) >= 0 && e.Iff == IFF_FOE) { p->Strips++; stripTimes[e.Dst].push_back({t, e.Src}); }
				else if (kConditions.count(e.Skill) && friends.count(e.Src))
				{
					if (e.Src == e.Dst) { p->CleansesSelf++; }
					else { p->Cleanses++; cleanseTimes[e.Dst].push_back({t, e.Src}); }
				}
				break;
			}
			case SC_AnimationStart:
				if (player(e.Src))
				{
					casts[e.Src].push_back({t - start, static_cast<int32_t>(e.Skill)});
					windows[e.Src].push_back({t, t + std::max({e.Value, e.BuffDmg, 0}) + kCastSlack, static_cast<int32_t>(e.Skill), false});
				}
				break;
			case SC_AnimationStop:
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
				else if (foes.count(e.Src)) { f.EnemyDowns++; f.EnemyDownMs.push_back(rel(t)); }
				break;
			case SC_ChangeDead:
				if (Player* p = player(e.Src))
				{
					p->Deaths++; f.SquadDeaths++; deadSince.emplace(e.Src, t); cannotActSince.emplace(e.Src, t);
					if (auto it = downOpen.find(e.Src); it != downOpen.end()) { p->DownSpans.push_back({rel(it->second), rel(t), false}); downOpen.erase(it); }
					deadOpen[e.Src] = t;
				}
				else if (foes.count(e.Src)) { f.EnemyDeaths++; }
				break;
			case SC_ChangeUp: case SC_Spawn:
				if (auto it = deadSince.find(e.Src); it != deadSince.end()) { deadMs[e.Src] += t - it->second; deadSince.erase(it); }
				if (auto it = cannotActSince.find(e.Src); it != cannotActSince.end()) { cannotAct[e.Src].push_back({it->second, t}); cannotActSince.erase(it); }
				if (Player* p = player(e.Src))
				{
					if (auto it = downOpen.find(e.Src); it != downOpen.end()) { p->DownSpans.push_back({rel(it->second), rel(t), false}); downOpen.erase(it); }
					if (auto it = deadOpen.find(e.Src); it != deadOpen.end()) { p->DownSpans.push_back({rel(it->second), rel(t), true}); deadOpen.erase(it); }
				}
				break;
			case SC_BuffApply: case SC_BuffInitial:
			{
				// a profession mechanic switched on: its buff of the same name on the player
				if (e.StateChange == SC_BuffApply && e.Src == e.Dst && player(e.Src) && useKind(e.Skill) == U_Mode) { noteUse(e.Src, e.Skill, t); }
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
				if (e.StateChange == SC_BuffApply && player(e.Src)) { boonApplied[e.Src][b].push_back(t); }
				bool counting = f.Intensity[b] || e.Shields;
				uint64_t key = stackKey(e.Dst, e.Pad61);
				stop(key, e.Dst, t);
				stacks[key] = {e.Src, b, t, t, counting};
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
					stabIndex.erase(it);
				}
				if (stacks.count(key)) { stop(key, e.Src, t); stacks.erase(key); }
				break;
			}
			default:
				break;
			}
		}
		for (auto& [key, st] : stacks) { stop(key, stackTarget[key], end); }
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
		f.OurSpikesMs = Spikes(f.OutPerS);
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

		// Which skills give which boons, from this round's log: pooled over everyone of the same profession (sister
		// specs share mechanics), a skill gives a boon when that player applied it within most of the skill's uses,
		// far more often than a random moment would show
		Givers learned;
		{
			struct Evidence { int Uses = 0; std::array<int, kBoons> Followed{}; std::array<double, kBoons> Expected{}; };
			std::unordered_map<uint64_t, Evidence> evidence;
			for (Player& p : f.Players)
			{
				const auto& w = windows[p.Addr];
				if (w.empty()) { continue; }
				auto& applied = boonApplied[p.Addr];
				double len = 0;
				for (const Window& x : w) { len += static_cast<double>(x.End - x.Start + kBoonDelay); }
				len = std::max(1.0, len / w.size());
				std::array<double, kBoons> chance{};
				for (int b = 0; b < kBoons; b++)
				{
					std::sort(applied[b].begin(), applied[b].end());
					int moments = 0;
					for (size_t i = 0; i < applied[b].size(); i++) { moments += i == 0 || applied[b][i] - applied[b][i - 1] > 100; }
					chance[b] = std::min(1.0, moments * len / std::max<int64_t>(1, f.DurationMs));
				}
				for (const Window& x : w)
				{
					if (x.Skill <= 0 && x.Skill > -20) { continue; } // 0 (other sources), weapon swap; negative ids below are EI's
					if (BoonIndex(static_cast<uint32_t>(x.Skill)) >= 0 || kConditions.count(static_cast<uint32_t>(x.Skill))) { continue; } // a boon isn't its own source
					Evidence& ev = evidence[GiverKey(p.ProfId, x.Skill)];
					ev.Uses++;
					for (int b = 0; b < kBoons; b++)
					{
						// during the use or shortly after (some skills grant their boons a moment later)
						auto it = std::lower_bound(applied[b].begin(), applied[b].end(), x.Start);
						ev.Followed[b] += it != applied[b].end() && *it <= x.End + kBoonDelay;
						ev.Expected[b] += chance[b] * (x.End + kBoonDelay - x.Start) / len;
					}
				}
			}
			for (auto& [key, ev] : evidence)
			{
				for (int b = 0; b < kBoons; b++)
				{
					if (ev.Uses >= 3 && ev.Followed[b] * 2 >= ev.Uses && ev.Followed[b] >= 3 * ev.Expected[b] + 1) { learned[key] |= 1u << b; }
				}
			}
			for (auto& [key, mask] : learned) { f.LearnedGivers.push_back({static_cast<uint32_t>(key >> 32), static_cast<int32_t>(key & 0xFFFFFFFF), mask}); }
		}

		// Boon generation and uptime
		std::unordered_map<uint64_t, std::array<double, kBoons>> onTarget;
		for (const Credit& c : credits)
		{
			onTarget[c.Target][c.Boon] += c.Ms;
			Player* src = player(c.Source);
			Player* tgt = player(c.Target);
			if (!src || !tgt || src == tgt) { continue; }
			double s = c.Ms / 1000.0;
			int32_t skill = AttributeBoon(windows[src->Addr], c.Applied, c.Boon, src->ProfId, learned);
			src->BoonSquadS[c.Boon] += s;
			src->Skills[skill].BoonSquadS[c.Boon] += s;
			if (src->Subgroup == tgt->Subgroup)
			{
				src->BoonGroupS[c.Boon] += s;
				src->Skills[skill].BoonGroupS[c.Boon] += s;
			}
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

		// Names for every skill a player has a row for
		for (const Player& p : f.Players)
		{
			for (auto& [skill, row] : p.Skills)
			{
				if (f.SkillNames.count(skill)) { continue; }
				std::string name;
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
		return f;
	}
}
