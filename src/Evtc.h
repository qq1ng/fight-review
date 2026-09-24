#pragma once
// Reader for ArcDPS .zevtc / .evtc combat logs (revision 1). Port of tools/evtc.py; layout per
// reference/arcdps_evtc_README.txt.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Evtc
{
	// cbtevent, revision 1: 64 bytes.
#pragma pack(push, 1)
	struct Event
	{
		uint64_t Time;
		uint64_t Src;
		uint64_t Dst;
		int32_t  Value;
		int32_t  BuffDmg;
		uint32_t Overstack;
		uint32_t Skill;
		uint16_t SrcInst;
		uint16_t DstInst;
		uint16_t SrcMaster;
		uint16_t DstMaster;
		uint8_t  Iff;
		uint8_t  Buff;
		uint8_t  Result;
		uint8_t  Activation;
		uint8_t  BuffRemove;
		uint8_t  Ninety;
		uint8_t  Fifty;
		uint8_t  Moving;
		uint8_t  StateChange;
		uint8_t  Flanking;
		uint8_t  Shields;
		uint8_t  Offcycle;
		uint32_t Pad61; // buff stack ("trackable") id for buff events
	};
#pragma pack(pop)
	static_assert(sizeof(Event) == 64, "cbtevent is 64 bytes");

	// Statechange values: the CBTS_ enum's position in the readme.
	enum SC : uint8_t
	{
		SC_None = 0,
		SC_EnterCombat = 1,
		SC_ExitCombat = 2,
		SC_ChangeUp = 3,
		SC_ChangeDead = 4,
		SC_ChangeDown = 5,
		SC_Spawn = 6,
		SC_Despawn = 7,
		SC_SqCombatStart = 9,
		SC_SqCombatEnd = 10,
		SC_WeapSwap = 11,
		SC_PointOfView = 13,
		SC_GwBuild = 15,
		SC_BuffInitial = 18,
		SC_BuffActive = 27,
		SC_BuffDeactive = 28,
		SC_BuffInfo = 30,
		SC_IdToGuid = 46,
		SC_ExtensionCombat = 49,
		SC_MissileCreate = 57,
		SC_EffectGroundCreate = 60,
		SC_EffectAgentCreate = 62,
		SC_AnimationStart = 67,
		SC_AnimationStop = 68,
		SC_BuffApply = 69,
		SC_BuffChange = 70,
		SC_BuffRemoveSingle = 71,
		SC_BuffRemoveAll = 72,
	};

	constexpr uint8_t IFF_FOE = 1;
	constexpr uint8_t RESULT_BLOCK = 3, RESULT_EVADE = 4, RESULT_ABSORB = 6;

	struct Agent
	{
		uint64_t    Addr = 0;
		uint32_t    Prof = 0;
		uint32_t    Elite = 0;
		bool        Player = false;
		std::string Name;
		std::string Account; // squad members and allies only; enemies in WvW have none
		int         Subgroup = 0;
	};

	struct Log
	{
		std::string                              ArcBuild;
		uint16_t                                 Species = 0;
		std::unordered_map<uint64_t, Agent>      Agents;
		std::unordered_map<int32_t, std::string> Skills;
		std::vector<Event>                       Events;

		const Agent* Find(uint64_t aAddr) const
		{
			auto it = Agents.find(aAddr);
			return it == Agents.end() ? nullptr : &it->second;
		}
		std::string SkillName(int32_t aId) const
		{
			auto it = Skills.find(aId);
			return it == Skills.end() ? std::string() : it->second;
		}
	};

	// Throws std::runtime_error on anything that isn't a readable revision 1 log.
	Log Load(const std::filesystem::path& aPath);

	const char* SpecName(const Agent& aAgent); // elite spec, else core profession
	const char* ProfessionName(uint32_t aProf);
}
