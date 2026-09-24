#include "InstantCasts.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>

namespace InstantCasts
{
	namespace
	{
		using namespace Evtc;

		enum RuleType
		{
			CR_BuffGain, CR_BuffGive, CR_BuffLoss, CR_MinionCommand, CR_Damage, CR_EXTHealing, CR_EXTBarrier,
			CR_Effect, CR_EffectByDst, CR_Missile, CR_MinionCast, CR_MinionSpawn
		};

		struct CastRule
		{
			RuleType    Type;
			int32_t     Skill;
			int64_t     Arg;          // buff, skill, species
			const char* Guid;         // effect GUID (hex), for effect rules
			int         Species[4];   // minion spawn rules
			int64_t     Icd;
			uint64_t    BuildMin, BuildMax;
			bool        Minions;
			int64_t     TimeOffset;
			int         DisabledWith; // 1: when the log has effect data, 2: missile data
			const char* Spec;         // "who[!][^]=Spec|Spec;..."  ! negates, ^ base profession
			int64_t     DurationMin, DurationMax;
			int         AroundDst;    // 1: must be around an agent, 2: must not
			const char* SecondGuid;   // another effect by the same agent at the same time
			int64_t     SecondOffset;
		};

#include "CastRules.inc"

		constexpr int64_t kServerDelay = 10;
		constexpr uint8_t kHealSelfReported = 128;

		struct Effect { int64_t Time; uint64_t Src; uint64_t Dst; int64_t Duration; };
		struct BuffApply { int64_t Time; uint64_t By; uint64_t To; int64_t Duration; };

		struct Context
		{
			const Log* L = nullptr;
			uint64_t Build = 0;
			std::unordered_map<uint64_t, uint64_t> Master; // minion -> player
			std::unordered_map<std::string, std::vector<Effect>> Effects;
			std::unordered_map<uint32_t, std::vector<BuffApply>> Applies;
			std::unordered_map<uint32_t, std::vector<std::pair<int64_t, uint64_t>>> Losses, Damage, Missiles, MinionCasts;
			std::unordered_map<uint32_t, std::vector<std::tuple<int64_t, uint64_t, bool>>> Heals;
			std::unordered_map<uint64_t, std::vector<int64_t>> Spawns;

			uint64_t FinalMaster(uint64_t a) const { auto it = Master.find(a); return it == Master.end() ? a : it->second; }
			bool IsPlayer(uint64_t a) const { const Agent* ag = L->Find(a); return ag && ag->Player; }
			int Species(uint64_t a) const
			{
				const Agent* ag = L->Find(a);
				if (!ag || ag->Player || (ag->Prof >> 16) == 0xFFFF) { return -1; }
				return static_cast<int>(ag->Prof & 0xFFFF);
			}
			std::string Spec(uint64_t a, bool aBase) const
			{
				const Agent* ag = L->Find(a);
				if (!ag || !ag->Player) { return ""; }
				return aBase ? ProfessionName(ag->Prof) : SpecName(*ag);
			}
		};

		std::string GuidHex(uint64_t aLow, uint64_t aHigh)
		{
			unsigned char bytes[16];
			std::memcpy(bytes, &aLow, 8);
			std::memcpy(bytes + 8, &aHigh, 8);
			char out[33];
			for (int i = 0; i < 16; i++) { std::snprintf(out + 2 * i, 3, "%02X", bytes[i]); }
			return std::string(out, 32);
		}

		Context Build(const Log& aLog)
		{
			Context c;
			c.L = &aLog;
			std::unordered_map<uint16_t, uint64_t> instToPlayer;
			std::unordered_map<uint32_t, std::string> guids;
			for (const Event& e : aLog.Events)
			{
				if (e.StateChange == SC_GwBuild && !c.Build) { c.Build = e.Src; }
				else if (e.StateChange == SC_IdToGuid) { guids[e.Skill] = GuidHex(e.Src, e.Dst); }
				else if (e.StateChange == SC_None && c.IsPlayer(e.Src)) { instToPlayer.emplace(e.SrcInst, e.Src); }
			}
			for (const Event& e : aLog.Events)
			{
				if (e.SrcMaster && !c.Master.count(e.Src) && !c.IsPlayer(e.Src))
				{
					if (auto it = instToPlayer.find(e.SrcMaster); it != instToPlayer.end()) { c.Master[e.Src] = it->second; }
				}
				if (e.DstMaster && !c.Master.count(e.Dst) && !c.IsPlayer(e.Dst))
				{
					if (auto it = instToPlayer.find(e.DstMaster); it != instToPlayer.end()) { c.Master[e.Dst] = it->second; }
				}
			}
			std::unordered_map<uint64_t, int64_t> firstSeen;
			for (const Event& e : aLog.Events)
			{
				const int64_t t = static_cast<int64_t>(e.Time);
				switch (e.StateChange)
				{
				case SC_None:
					c.Damage[e.Skill].push_back({t, e.Src});
					firstSeen.emplace(e.Src, t);
					break;
				case SC_EffectAgentCreate: case SC_EffectGroundCreate:
				{
					auto it = guids.find(e.Skill);
					if (it == guids.end()) { break; }
					int64_t duration = e.Iff | (e.Buff << 8) | (e.Result << 16) | (static_cast<int64_t>(e.Activation) << 24);
					c.Effects[it->second].push_back({t, e.Src, e.StateChange == SC_EffectAgentCreate ? e.Dst : 0, duration});
					break;
				}
				case SC_BuffApply: c.Applies[e.Skill].push_back({t, e.Src, e.Dst, e.Value}); break;
				case SC_BuffRemoveAll: c.Losses[e.Skill].push_back({t, e.Src}); break;
				case SC_ExtensionCombat:
					if (e.Offcycle & kHealSelfReported) { c.Heals[e.Skill].push_back({t, e.Src, e.Shields != 0}); }
					break;
				case SC_MissileCreate: c.Missiles[e.Skill].push_back({t, e.Src}); break;
				case SC_AnimationStart: if (c.Master.count(e.Src)) { c.MinionCasts[e.Skill].push_back({t, e.Src}); } break;
				case SC_Spawn: c.Spawns[e.Src].push_back(t); break;
				default: break;
				}
			}
			for (auto& [minion, master] : c.Master)
			{
				if (!c.Spawns.count(minion)) { if (auto it = firstSeen.find(minion); it != firstSeen.end()) { c.Spawns[minion].push_back(it->second); } }
			}
			return c;
		}

		// "who[!][^]=A|B;..." against the agents playing each part
		bool SpecOk(const CastRule& r, const Context& c, uint64_t aSrc, uint64_t aDst)
		{
			if (!r.Spec[0]) { return true; }
			std::string all = r.Spec;
			size_t pos = 0;
			while (pos < all.size())
			{
				size_t end = all.find(';', pos);
				std::string part = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
				pos = end == std::string::npos ? all.size() : end + 1;
				size_t eq = part.find('=');
				std::string who = part.substr(0, eq), specs = "|" + part.substr(eq + 1) + "|";
				bool negate = who.find('!') != std::string::npos, base = who.find('^') != std::string::npos;
				who.erase(std::remove_if(who.begin(), who.end(), [](char ch) { return ch == '!' || ch == '^'; }), who.end());
				uint64_t agent = who == "src" || who == "by" ? aSrc : aDst;
				std::string spec = agent ? c.Spec(agent, base) : "";
				bool in = !spec.empty() && specs.find("|" + spec + "|") != std::string::npos;
				if (in == negate) { return false; }
			}
			return true;
		}

		template <typename Emit>
		void Matches(const CastRule& r, const Context& c, Emit&& emit)
		{
			switch (r.Type)
			{
			case CR_BuffGain: case CR_BuffGive: case CR_MinionCommand:
			{
				uint32_t buff = r.Type == CR_MinionCommand ? kMinionCommandBuff : static_cast<uint32_t>(r.Arg);
				auto it = c.Applies.find(buff);
				if (it == c.Applies.end()) { return; }
				for (const BuffApply& a : it->second)
				{
					if (r.Type == CR_MinionCommand && (c.Species(a.To) != r.Arg || !c.Master.count(a.To))) { continue; }
					if (r.DurationMin >= 0 && std::llabs(a.Duration - r.DurationMin) >= kServerDelay) { continue; }
					if (!SpecOk(r, c, a.By, a.To)) { continue; }
					uint64_t key = r.Type == CR_BuffGive ? a.By : a.To;
					emit(a.Time, (r.Minions || r.Type != CR_BuffGain) ? c.FinalMaster(key) : key);
				}
				return;
			}
			case CR_BuffLoss:
				if (auto it = c.Losses.find(static_cast<uint32_t>(r.Arg)); it != c.Losses.end())
				{
					for (auto& [t, to] : it->second) { if (SpecOk(r, c, 0, to)) { emit(t, r.Minions ? c.FinalMaster(to) : to); } }
				}
				return;
			case CR_Damage:
				if (auto it = c.Damage.find(static_cast<uint32_t>(r.Arg)); it != c.Damage.end())
				{
					for (auto& [t, src] : it->second) { emit(t, src); }
				}
				return;
			case CR_EXTHealing: case CR_EXTBarrier:
				if (auto it = c.Heals.find(static_cast<uint32_t>(r.Arg)); it != c.Heals.end())
				{
					for (auto& [t, src, barrier] : it->second) { if (barrier == (r.Type == CR_EXTBarrier)) { emit(t, src); } }
				}
				return;
			case CR_Effect: case CR_EffectByDst:
			{
				auto it = c.Effects.find(r.Guid);
				if (it == c.Effects.end()) { return; }
				bool byDst = r.Type == CR_EffectByDst;
				for (const Effect& fx : it->second)
				{
					uint64_t key = byDst ? fx.Dst : fx.Src;
					if (!key || !SpecOk(r, c, fx.Src, fx.Dst)) { continue; }
					if (r.AroundDst && ((fx.Dst != 0) != (r.AroundDst == 1))) { continue; }
					if (r.DurationMin >= 0 && (fx.Duration < r.DurationMin || fx.Duration > r.DurationMax)) { continue; }
					if (r.SecondGuid[0])
					{
						auto other = c.Effects.find(r.SecondGuid);
						bool found = false;
						if (other != c.Effects.end())
						{
							for (const Effect& o : other->second)
							{
								if (&o == &fx) { continue; }
								if ((byDst ? o.Dst : o.Src) == key && std::llabs(o.Time - r.SecondOffset - fx.Time) < kServerDelay) { found = true; break; }
							}
						}
						if (!found) { continue; }
					}
					emit(fx.Time, r.Minions ? c.FinalMaster(key) : key);
				}
				return;
			}
			case CR_Missile:
				if (auto it = c.Missiles.find(static_cast<uint32_t>(r.Arg)); it != c.Missiles.end())
				{
					for (auto& [t, src] : it->second) { emit(t, r.Minions ? c.FinalMaster(src) : src); }
				}
				return;
			case CR_MinionCast:
				if (auto it = c.MinionCasts.find(static_cast<uint32_t>(r.Arg)); it != c.MinionCasts.end())
				{
					for (auto& [t, minion] : it->second) { emit(t, c.FinalMaster(minion)); }
				}
				return;
			case CR_MinionSpawn:
				for (auto& [minion, master] : c.Master)
				{
					int sp = c.Species(minion);
					if (sp < 0 || std::find(std::begin(r.Species), std::end(r.Species), sp) == std::end(r.Species) || sp == 0) { continue; }
					if (auto it = c.Spawns.find(minion); it != c.Spawns.end()) { for (int64_t t : it->second) { emit(t, master); } }
				}
				return;
			}
		}
	}

	std::unordered_map<uint64_t, std::vector<std::pair<int64_t, int32_t>>> Find(const Log& aLog)
	{
		Context c = Build(aLog);
		bool hasEffects = !c.Effects.empty(), hasMissiles = !c.Missiles.empty();
		std::unordered_map<uint64_t, std::vector<std::pair<int64_t, int32_t>>> out;
		std::vector<std::pair<int64_t, uint64_t>> found;
		for (const CastRule& r : kCastRules)
		{
			if (c.Build < r.BuildMin || c.Build >= r.BuildMax) { continue; }
			if ((r.DisabledWith == 1 && hasEffects) || (r.DisabledWith == 2 && hasMissiles)) { continue; }
			found.clear();
			Matches(r, c, [&](int64_t t, uint64_t caster) { found.push_back({t, caster}); });
			std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
			std::unordered_map<uint64_t, int64_t> last;
			for (auto& [t, caster] : found)
			{
				if (!caster || !c.IsPlayer(caster)) { continue; }
				auto it = last.find(caster);
				bool within = it != last.end() && t - it->second < r.Icd;
				last[caster] = t;
				if (!within) { out[caster].push_back({t + r.TimeOffset, r.Skill}); }
			}
		}
		return out;
	}

	const char* Name(int32_t aSkill, bool aFallback)
	{
		if (!aFallback) { for (auto& [id, name] : kSkillNames) { if (id == aSkill) { return name; } } }
		else { for (auto& [id, name] : kFallbackSkillNames) { if (id == aSkill) { return name; } } }
		return nullptr;
	}
}
