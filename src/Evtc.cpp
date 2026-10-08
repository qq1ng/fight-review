#include "Evtc.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "miniz/miniz.h"

namespace Evtc
{
	namespace
	{
		constexpr uint32_t kNotPlayer = 0xFFFFFFFF;
#include "SkillNamesEn.inc"

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

	// A log inside an archive, given as "<archive.zip>\<member>" (tools/import_logs.py: other players' logs stay in their
	// zip, 30 GB of them on 2026-10-08, nothing unpacked): the member's bytes, read from the archive on disk. Empty when the
	// path isn't one.
	std::vector<uint8_t> FromArchive(const std::filesystem::path& aPath)
	{
		if (std::filesystem::exists(aPath)) { return {}; }
		for (std::filesystem::path zip = aPath.parent_path(); !zip.empty() && zip != zip.root_path(); zip = zip.parent_path())
		{
			std::error_code ec;
			if (!std::filesystem::is_regular_file(zip, ec)) { continue; }
			const std::string member = std::filesystem::relative(aPath, zip, ec).generic_string(); // "a/b.zevtc"
			// read through a stream (miniz is built without its file functions): only the directory and this entry are read
			std::ifstream in(zip, std::ios::binary);
			mz_zip_archive z{};
			z.m_pIO_opaque = &in;
			z.m_pRead = [](void* aOpaque, mz_uint64 aOfs, void* aBuf, size_t aN) -> size_t
			{
				auto& s = *static_cast<std::ifstream*>(aOpaque);
				s.clear();
				s.seekg(static_cast<std::streamoff>(aOfs));
				s.read(static_cast<char*>(aBuf), static_cast<std::streamsize>(aN));
				return static_cast<size_t>(s.gcount());
			};
			if (!in || !mz_zip_reader_init(&z, std::filesystem::file_size(zip, ec), 0)) { throw std::runtime_error("cannot open " + zip.string()); }
			size_t size = 0;
			void* p = mz_zip_reader_extract_file_to_heap(&z, member.c_str(), &size, 0);
			mz_zip_reader_end(&z);
			if (!p) { throw std::runtime_error("no " + member + " in " + zip.string()); }
			std::vector<uint8_t> out(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
			mz_free(p);
			return out;
		}
		return {};
	}

	Log Load(const std::filesystem::path& aPath)
	{
		std::vector<uint8_t> data = FromArchive(aPath);
		if (data.empty()) { data = ReadFile(aPath); }
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

		// ArcDPS up to its 2026-03-17 build (the next one seen is 2026-05-07) wrote buff applies, duration changes and
		// removals as combat events with is_buff set: an apply has value (its duration) and no is_buffremove, a duration
		// change also has is_offcycle (no source, a stack already there, overstack_value its new duration), a removal has
		// is_buffremove (1 every stack, 2 one stack, 3 one stack split off a remove-all). Later builds write them as their
		// own statechanges with the same fields, so the old ones are relabelled here and everything after reads one format.
		// A buff's damage (value 0, buff_dmg set) stays a combat event; healing addon events are statechange 49 in both.
		// (7,064 logs from 2024-07 on: every build up to 20260317 the old way, every one since the new way.)
		for (Event& e : log.Events)
		{
			if (e.StateChange != SC_None || !e.Buff || e.Activation) { continue; }
			if (e.BuffRemove) { e.StateChange = e.BuffRemove == 1 ? SC_BuffRemoveAll : SC_BuffRemoveSingle; }
			else if (e.Value != 0) { e.StateChange = e.Offcycle ? SC_BuffChange : SC_BuffApply; }
		}

		// A client in another language names skills and buffs in it ("Relique du héraut"); the analysis finds some buffs by
		// their English name (Illusion of Life, Stun, a Revenant's legend) and the UI is English, so known ids get the
		// English name (tools/gen_skill_names.py: from English logs and the GW2 API)
		for (const Event& e : log.Events)
		{
			if (e.StateChange != SC_Language) { continue; }
			if (e.Src != 0)
			{
				for (auto& [id, name] : log.Skills)
				{
					auto it = std::lower_bound(std::begin(kSkillNamesEn), std::end(kSkillNamesEn), id, [](const SkillNameEn& a, int32_t b) { return a.Skill < b; });
					if (it != std::end(kSkillNamesEn) && it->Skill == id) { name = it->Name; }
				}
			}
			break;
		}
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
