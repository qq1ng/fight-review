#include "Evtc.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

#include "miniz/miniz.h"

namespace Evtc
{
	namespace
	{
		constexpr uint32_t kNotPlayer = 0xFFFFFFFF;

		std::vector<uint8_t> ReadFile(const std::filesystem::path& aPath)
		{
			std::ifstream in(aPath, std::ios::binary);
			if (!in) { throw std::runtime_error("cannot open " + aPath.string()); }
			return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}

		std::vector<uint8_t> Unzip(const std::vector<uint8_t>& aZip)
		{
			mz_zip_archive zip{};
			if (!mz_zip_reader_init_mem(&zip, aZip.data(), aZip.size(), 0)) { throw std::runtime_error("not a zip"); }
			size_t size = 0;
			void* data = mz_zip_reader_extract_to_heap(&zip, 0, &size, 0);
			mz_zip_reader_end(&zip);
			if (!data) { throw std::runtime_error("cannot inflate the log"); }
			std::vector<uint8_t> out(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
			mz_free(data);
			return out;
		}

		template <typename T> T Read(const std::vector<uint8_t>& aData, size_t aPos)
		{
			if (aPos + sizeof(T) > aData.size()) { throw std::runtime_error("truncated log"); }
			T value;
			std::memcpy(&value, aData.data() + aPos, sizeof(T));
			return value;
		}

		// "character\0:account\0subgroup\0" for players
		std::vector<std::string> SplitName(const char* aRaw, size_t aLen)
		{
			std::vector<std::string> parts;
			size_t start = 0;
			for (size_t i = 0; i <= aLen; i++)
			{
				if (i == aLen || aRaw[i] == '\0')
				{
					parts.emplace_back(aRaw + start, i - start);
					start = i + 1;
					if (parts.size() == 3) { break; }
				}
			}
			return parts;
		}
	}

	Log Load(const std::filesystem::path& aPath)
	{
		std::vector<uint8_t> data = ReadFile(aPath);
		if (aPath.extension() == ".zevtc") { data = Unzip(data); }
		if (data.size() < 16 || std::memcmp(data.data(), "EVTC", 4) != 0) { throw std::runtime_error("not an evtc file"); }
		Log log;
		log.ArcBuild.assign(reinterpret_cast<const char*>(data.data()) + 4, 8);
		if (data[12] != 1) { throw std::runtime_error("unsupported evtc revision"); }
		log.Species = Read<uint16_t>(data, 13);

		size_t pos = 16;
		uint32_t agents = Read<uint32_t>(data, pos);
		pos += 4;
		for (uint32_t i = 0; i < agents; i++, pos += 96)
		{
			Agent a;
			a.Addr = Read<uint64_t>(data, pos);
			a.Prof = Read<uint32_t>(data, pos + 8);
			a.Elite = Read<uint32_t>(data, pos + 12);
			a.Player = a.Elite != kNotPlayer;
			if (pos + 96 > data.size()) { throw std::runtime_error("truncated agent table"); }
			auto parts = SplitName(reinterpret_cast<const char*>(data.data()) + pos + 28, 64);
			a.Name = parts.size() > 0 ? parts[0] : "";
			if (a.Player && parts.size() > 1)
			{
				a.Account = parts[1];
				if (!a.Account.empty() && a.Account[0] == ':') { a.Account.erase(0, 1); }
				if (parts.size() > 2 && !parts[2].empty() && isdigit(static_cast<unsigned char>(parts[2][0])))
				{
					a.Subgroup = std::atoi(parts[2].c_str());
				}
			}
			log.Agents[a.Addr] = std::move(a);
		}

		uint32_t skills = Read<uint32_t>(data, pos);
		pos += 4;
		for (uint32_t i = 0; i < skills; i++, pos += 68)
		{
			int32_t id = Read<int32_t>(data, pos);
			if (pos + 68 > data.size()) { throw std::runtime_error("truncated skill table"); }
			const char* name = reinterpret_cast<const char*>(data.data()) + pos + 4;
			log.Skills[id] = std::string(name, strnlen(name, 64));
		}

		size_t count = (data.size() - pos) / sizeof(Event);
		log.Events.resize(count);
		if (count) { std::memcpy(log.Events.data(), data.data() + pos, count * sizeof(Event)); }
		return log;
	}

	const char* ProfessionName(uint32_t aProf)
	{
		static const char* kNames[] = {"?", "Guardian", "Warrior", "Engineer", "Ranger", "Thief", "Elementalist",
			"Mesmer", "Necromancer", "Revenant"};
		return aProf < 10 ? kNames[aProf] : "?";
	}

	const char* SpecName(const Agent& aAgent)
	{
		switch (aAgent.Elite)
		{
		case 27: return "Dragonhunter"; case 62: return "Firebrand"; case 65: return "Willbender"; case 81: return "Luminary";
		case 18: return "Berserker"; case 61: return "Spellbreaker"; case 68: return "Bladesworn"; case 74: return "Paragon";
		case 43: return "Scrapper"; case 57: return "Holosmith"; case 70: return "Mechanist"; case 75: return "Amalgam";
		case 5: return "Druid"; case 55: return "Soulbeast"; case 72: return "Untamed"; case 78: return "Galeshot";
		case 7: return "Daredevil"; case 58: return "Deadeye"; case 71: return "Specter"; case 77: return "Antiquary";
		case 48: return "Tempest"; case 56: return "Weaver"; case 67: return "Catalyst"; case 80: return "Evoker";
		case 40: return "Chronomancer"; case 59: return "Mirage"; case 66: return "Virtuoso"; case 73: return "Troubadour";
		case 34: return "Reaper"; case 60: return "Scourge"; case 64: return "Harbinger"; case 76: return "Ritualist";
		case 52: return "Herald"; case 63: return "Renegade"; case 69: return "Vindicator"; case 79: return "Conduit";
		default: return ProfessionName(aAgent.Prof);
		}
	}
}
