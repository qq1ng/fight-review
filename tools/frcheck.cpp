// Prints the C++ analysis of one log as tab-separated lines, for tools/check_cpp.py to compare with the Python
// prototype. Usage: frcheck <file.zevtc>
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <exception>

#include "Analysis.h"

// --pool: how much of each boon goes to "other sources" (skill 0) when each round learns only from itself,
// against learning from all the given rounds pooled (each round attributed with the others' evidence plus its own)
int Pool(int argc, char** argv)
{
	std::vector<std::string> paths(argv + 2, argv + argc);
	std::array<double, Analysis::kBoons> total{}, alone{}, pooledOther{}, pooledTraits{};
	std::map<std::string, double> traitS; // "trait / boon / spec" -> seconds given to others
	Analysis::Evidence all;
	std::vector<Analysis::Evidence> own;
	for (const std::string& path : paths)
	{
		Analysis::Fight f = Analysis::Analyse(path);
		for (const auto& p : f.Players)
		{
			for (int b = 0; b < Analysis::kBoons; b++)
			{
				total[b] += p.BoonSquadS[b];
				auto it = p.Skills.find(0);
				if (it != p.Skills.end()) { alone[b] += it->second.BoonSquadS[b]; }
			}
		}
		own.push_back(f.OwnEvidence);
		Analysis::AddEvidence(all, f.OwnEvidence);
	}
	size_t learnedPooled = 0;
	std::map<std::string, std::pair<double, double>> specStab; // spec -> stability given, of it from other sources
	std::map<std::string, int32_t> pulses;                      // skill / boon / profession -> reach
	std::map<std::string, double> fbStab;                       // Firebrand stability by skill
	for (size_t i = 0; i < paths.size(); i++)
	{
		Analysis::Evidence others = all;
		Analysis::AddEvidence(others, own[i], -1);
		Analysis::Fight f = Analysis::Analyse(paths[i], &others);
		learnedPooled = std::max(learnedPooled, f.LearnedGivers.size());
		for (const auto& pu : f.Pulses)
		{
			auto name = f.SkillNames.count(pu.Skill) ? f.SkillNames.at(pu.Skill) : std::to_string(pu.Skill);
			pulses[name + " / " + Analysis::kBoonNames[pu.Boon] + " / prof " + std::to_string(pu.Profession)] = pu.Ms;
		}
		for (const auto& p : f.Players)
		{
			specStab[p.Spec].first += p.BoonSquadS[Analysis::kStability];
			if (p.Spec == "Firebrand")
			{
				for (auto& [skill, row] : p.Skills)
				{
					if (row.BoonSquadS[Analysis::kStability] <= 0) { continue; }
					fbStab[f.SkillNames.count(skill) ? f.SkillNames.at(skill) : std::to_string(skill)] += row.BoonSquadS[Analysis::kStability];
				}
			}
			for (auto& [skill, row] : p.Skills)
			{
				if (skill > Analysis::kTraitBase) { continue; }
				for (int b = 0; b < Analysis::kBoons; b++)
				{
					pooledTraits[b] += row.BoonSquadS[b];
					if (row.BoonSquadS[b] > 0) { traitS[f.SkillNames[skill] + " / " + Analysis::kBoonNames[b] + " / " + p.Spec] += row.BoonSquadS[b]; }
				}
			}
			auto it = p.Skills.find(0);
			if (it == p.Skills.end()) { continue; }
			for (int b = 0; b < Analysis::kBoons; b++) { pooledOther[b] += it->second.BoonSquadS[b]; }
			specStab[p.Spec].second += it->second.BoonSquadS[Analysis::kStability];
		}
	}
	for (auto& [what, ms] : pulses) { std::printf("pulse\t%-60s %d ms\n", what.c_str(), ms); }
	for (auto& [name, v] : fbStab) { if (v > 500) { std::printf("firebrand stability\t%-45s %8.0f s\n", name.c_str(), v); } }
	for (auto& [spec, v] : specStab)
	{
		if (v.first > 1000) { std::printf("stability\t%-14s %8.0f s given, %5.1f%% other sources\n", spec.c_str(), v.first, 100 * v.second / v.first); }
	}
	std::printf("%zu rounds; %zu skill-profession pairs seen, most learned player-skill givers in a round %zu\n", paths.size(), all.ByProfession.size(), learnedPooled);
	std::printf("%-14s %10s %22s %22s %18s\n", "boon", "given s", "other sources, alone", "other sources, pooled", "traits, pooled");
	for (int b = 0; b < Analysis::kBoons; b++)
	{
		if (total[b] <= 0) { continue; }
		std::printf("%-14s %10.0f %21.1f%% %21.1f%% %17.1f%%\n", Analysis::kBoonNames[b], total[b], 100 * alone[b] / total[b], 100 * pooledOther[b] / total[b],
			100 * pooledTraits[b] / total[b]);
	}
	std::vector<std::pair<double, std::string>> traits;
	for (auto& [k, v] : traitS) { traits.push_back({v, k}); }
	std::sort(traits.rbegin(), traits.rend());
	for (size_t i = 0; i < traits.size() && i < 25; i++) { std::printf("trait\t%-60s %8.0f s\n", traits[i].second.c_str(), traits[i].first); }
	return 0;
}

// --series: per round, our damage to enemy players and the enemy's damage to us per second, and the downs on each
// side (ms from fight start), for tuning spike detection (tools/spike_sweep.py)
int Series(int argc, char** argv)
{
	for (int i = 2; i < argc; i++)
	{
		Analysis::Fight f = Analysis::Analyse(argv[i]);
		std::printf("round\t%s\t%lld\n", f.Stamp.c_str(), static_cast<long long>(f.DurationMs));
		std::printf("out");
		for (int64_t v : f.ToPlayersPerS) { std::printf("\t%lld", static_cast<long long>(v)); }
		std::printf("\nin");
		for (int64_t v : f.InPerS) { std::printf("\t%lld", static_cast<long long>(v)); }
		std::printf("\nenemydowns");
		for (int32_t v : f.EnemyDownMs) { std::printf("\t%d", v); }
		std::printf("\nsquaddowns");
		for (int32_t v : f.SquadDownMs) { std::printf("\t%d", v); }
		std::printf("\n");
	}
	return 0;
}

// --spikes: per our spike, each player's damage to enemy players from 3 s before to 3 s after its peak, when their
// damage there centred (damage-weighted mean, ms from the peak), their biggest skill in it, and whether they were
// CC'd or down in the 3 s before (for designing the spike breakdown)
int Spikes(int argc, char** argv)
{
	for (int i = 2; i < argc; i++)
	{
		Analysis::Fight f = Analysis::Analyse(argv[i]);
		for (int64_t s : f.OurSpikesMs)
		{
			std::printf("spike\t%s\t%lld\n", f.Stamp.c_str(), static_cast<long long>(s));
			for (const auto& p : f.Players)
			{
				double dmg = 0, weighted = 0;
				std::map<int32_t, double> bySkill;
				for (const auto& h : p.HitsOut)
				{
					if (h.Ms < s - 3000 || h.Ms > s + 3000) { continue; }
					dmg += h.Damage; weighted += double(h.Damage) * (h.Ms - s); bySkill[h.Skill] += h.Damage;
				}
				bool cc = false, down = false;
				for (int32_t t : p.CcMs) { cc |= t >= s - 3000 && t <= s; }
				for (const auto& d : p.DownSpans) { down |= d.From <= s && d.To >= s - 3000; }
				int32_t top = 0;
				for (auto& [sk, v] : bySkill) { if (!top || v > bySkill[top]) { top = sk; } }
				auto name = f.SkillNames.count(top) ? f.SkillNames.at(top) : std::to_string(top);
				std::printf("  %-14s %8.0f dmg  centre %+6.0f ms  top %-28s %s%s\n", p.Spec.c_str(), dmg, dmg > 0 ? weighted / dmg : 0.0,
					dmg > 0 ? name.c_str() : "-", cc ? "CC'd " : "", down ? "down" : "");
			}
		}
	}
	return 0;
}

// --credited <boon index> <logs...>: seconds of that boon given to the own subgroup, per spec and skill it was credited
// to, over all the logs (no names)
int Credited(int argc, char** argv)
{
	int boon = std::atoi(argv[2]);
	std::map<std::string, double> by;
	for (int i = 3; i < argc; i++)
	{
		Analysis::Fight f = Analysis::Analyse(argv[i]);
		for (const auto& p : f.Players)
		{
			for (const auto& [skill, row] : p.Skills)
			{
				if (row.BoonGroupS[boon] <= 0) { continue; }
				auto n = f.SkillNames.find(skill);
				by[p.Spec + "\t" + (n == f.SkillNames.end() ? std::to_string(skill) : n->second)] += row.BoonGroupS[boon];
			}
		}
	}
	std::vector<std::pair<double, std::string>> list;
	for (auto& [k, s] : by) { list.push_back({s, k}); }
	std::sort(list.rbegin(), list.rend());
	for (auto& [s, k] : list) { std::printf("%10.1f\t%s\n", s, k.c_str()); }
	return 0;
}

// --others <spec> <boon index> <logs...>: each moment a player on that spec gave the boon to someone else with no
// skill found for it ("other sources"), as the log's own time: "other <log> <agent address> <time>" (no names)
int Others(int argc, char** argv)
{
	std::string spec = argv[2];
	int boon = std::atoi(argv[3]);
	for (int i = 4; i < argc; i++)
	{
		Analysis::Fight f = Analysis::Analyse(argv[i]);
		for (const auto& p : f.Players)
		{
			if (p.Spec != spec) { continue; }
			int32_t last = INT32_MIN / 2;
			for (auto& [ms, b] : p.OtherBoonMs)
			{
				if (b != boon || ms - last < 150) { continue; }
				last = ms;
				std::printf("other\t%s\t%llu\t%lld\n", argv[i], static_cast<unsigned long long>(p.Addr), static_cast<long long>(f.LogStart + ms));
			}
		}
	}
	return 0;
}

// --selfstab <logs...>: per spec and skill, the stability moments credited to it and how many reached anyone but the
// giver (a skill that never does gives stability to the caster only)
int SelfStab(int argc, char** argv)
{
	std::map<std::string, std::pair<int, int>> by;
	for (int i = 2; i < argc; i++)
	{
		Analysis::Fight f = Analysis::Analyse(argv[i]);
		for (size_t k = 0; k < f.Players.size(); k++)
		{
			const auto& p = f.Players[k];
			for (const auto& g : p.StabGives)
			{
				auto n = f.SkillNames.find(g.Skill);
				auto& e = by[p.Spec + "\t" + (n == f.SkillNames.end() ? std::to_string(g.Skill) : n->second)];
				e.first++;
				e.second += std::any_of(g.Targets.begin(), g.Targets.end(), [&](int t) { return t != static_cast<int>(k); });
			}
		}
	}
	for (auto& [k, v] : by) { std::printf("%6d moments, %6d reached others (%3d%%)\t%s\n", v.first, v.second, v.first ? 100 * v.second / v.first : 0, k.c_str()); }
	return 0;
}

// Stability givers: presence on allies (nominal), TopStats' redundancy, and their stability on the subgroup at enemy
// spikes. One line per giver: stamp, character, spec, ally s, redundancy %, spikes, share at spikes %
int StabRed(int argc, char** argv)
{
	for (int i = 2; i < argc; i++)
	{
		Analysis::Fight f = Analysis::Analyse(argv[i]);
		for (const auto& p : f.Players)
		{
			if (p.StabAllyNominalMs < 5000) { continue; }
			std::printf("%s\t%s\t%s\t%.1f\t%.1f\t%d\t%.0f\n", f.Stamp.c_str(), p.Name.c_str(), p.Spec.c_str(), p.StabAllyNominalMs / 1000.0,
				100.0 * double(p.StabRedundantMs) / double(p.StabAllyNominalMs), p.StabSpikes, p.StabSpikes ? 100.0 * p.StabSpikeShare / p.StabSpikes : -1.0);
		}
	}
	return 0;
}

int main(int argc, char** argv)
{
	if (argc >= 3 && std::string(argv[1]) == "--stabred") { return StabRed(argc, argv); }
	if (argc >= 5 && std::string(argv[1]) == "--others") { return Others(argc, argv); }
	if (argc >= 3 && std::string(argv[1]) == "--selfstab") { return SelfStab(argc, argv); }
	if (argc >= 2 && std::string(argv[1]) == "--spikes") { return Spikes(argc, argv); }
	if (argc >= 4 && std::string(argv[1]) == "--credited") { return Credited(argc, argv); }
	// --evidence <skill id> <logs...>: the pooled evidence that a skill gives stability (per profession and per player)
	if (argc >= 4 && std::string(argv[1]) == "--evidence")
	{
		int32_t skill = std::atoi(argv[2]);
		Analysis::Evidence all;
		for (int i = 3; i < argc; i++) { Analysis::AddEvidence(all, Analysis::Analyse(argv[i]).OwnEvidence); }
		auto show = [&](const char* aWho, const Analysis::BoonEvidence& ev)
		{
			std::printf("%-12s uses %4d  stability followed %4d, expected %6.1f  might %d/%.1f  quickness %d/%.1f\n", aWho, ev.Uses, ev.Followed[Analysis::kStability],
				ev.Expected[Analysis::kStability], ev.Followed[0], ev.Expected[0], ev.Followed[2], ev.Expected[2]);
		};
		for (auto& [key, ev] : all.ByProfession) { if (static_cast<int32_t>(key & 0xFFFFFFFF) == skill) { show(("prof " + std::to_string(key >> 32)).c_str(), ev); } }
		int n = 0;
		for (auto& [key, ev] : all.ByPlayer) { if (key.second == skill) { show(("player " + std::to_string(++n)).c_str(), ev); } }
		return 0;
	}
	// --gives <log> <name part> <from s> <to s>: each stability moment that player gave (their name, not published)
	if (argc == 6 && std::string(argv[1]) == "--gives")
	{
		Analysis::Fight f = Analysis::Analyse(argv[2]);
		int32_t a = static_cast<int32_t>(std::atof(argv[4]) * 1000), b = static_cast<int32_t>(std::atof(argv[5]) * 1000);
		for (const auto& p : f.Players)
		{
			if (p.Name.find(argv[3]) == std::string::npos) { continue; }
			for (const auto& g : p.StabGives)
			{
				if (g.Ms < a || g.Ms > b) { continue; }
				auto it = f.SkillNames.find(g.Skill);
				std::printf("%8d ms  %-40s %6d  %zu targets\n", g.Ms, it == f.SkillNames.end() ? std::to_string(g.Skill).c_str() : it->second.c_str(), g.Skill, g.Targets.size());
			}
		}
		return 0;
	}
	if (argc >= 2 && std::string(argv[1]) == "--series") { return Series(argc, argv); }
	if (argc < 2) { std::fprintf(stderr, "usage: frcheck <file.zevtc> | frcheck --pool <file.zevtc> ...\n"); return 2; }
	if (std::string(argv[1]) == "--pool") { return Pool(argc, argv); }
	try
	{
		Analysis::Fight f = Analysis::Analyse(argv[1]);
		std::printf("fight\t%lld\t%d\t%d\t%d\t%d\t%d\t%d\n", static_cast<long long>(f.DurationMs), f.SquadCount,
			f.EnemyCount, f.SquadDowns, f.SquadDeaths, f.EnemyDowns, f.EnemyDeaths);
		for (const auto& p : f.Players)
		{
			std::printf("player\t%s\t%s\t%d\t%d\t%lld\t%lld\t%lld\t%lld\t%lld\t%d\t%d\t%d\t%d\t%d\t%lld",
				p.Account.c_str(), p.Spec.c_str(), p.Subgroup, p.HealKnown ? 1 : 0, static_cast<long long>(p.Heal),
				static_cast<long long>(p.Barrier), static_cast<long long>(p.Damage), static_cast<long long>(p.DamageAll),
				static_cast<long long>(p.ActiveMs), p.Strips, p.Cleanses, p.Evades, p.Blocks, p.Invulns, 0LL);
			for (int b = 0; b < Analysis::kBoons; b++) { std::printf("\t%.4f", f.SquadGeneration(p, b)); }
			int casts = 0;
			for (auto& [skill, row] : p.Skills) { casts += row.Casts; }
			std::printf("\t%d\n", casts);
		}
		for (const auto& p : f.Players)
		{
			// What happened to the player (tools/check_topstats_cpp.py compares these with TopStats)
			int corrupted = 0;
			for (const auto& st : p.StripsIn) { corrupted += st.Corrupted; }
			std::printf("events\t%s\t%d\t%d\t%d\t%lld\t%lld\t%d\t%u\t%lld\t%zu\t%d\t%zu\n", p.Account.c_str(), p.Downs, p.Deaths, p.CcTaken,
				static_cast<long long>(p.DamageTaken), static_cast<long long>(p.ReviveMs), p.CcDealt, p.Legends,
				static_cast<long long>(p.DownContribution), p.DodgeMs.size(), corrupted, p.StripsIn.size());
		}
		for (const auto& p : f.Players)
		{
			std::printf("stab\t%s\t%s\t%d\t%d\t%d\t%lld\t%lld\t%lld\n", p.Account.c_str(), p.Name.c_str(), p.StabEligible,
				p.StabCovered, p.StabReady, static_cast<long long>(p.StabAllyMs), static_cast<long long>(p.StabSelfMs),
				static_cast<long long>(p.ActiveMs));
		}
		for (const auto& p : f.Players)
		{
			for (auto& [skill, row] : p.Skills)
			{
				if (row.Casts) { std::printf("skill\t%s\t%d\t%d\n", p.Account.c_str(), skill, row.Casts); }
			}
		}
		std::printf("spikes\t%zu\t%zu\n", f.OurSpikesMs.size(), f.TheirSpikesMs.size());
		for (const auto& g : f.LearnedGivers)
		{
			std::string boons;
			for (int b = 0; b < Analysis::kBoons; b++) { if (g.Boons >> b & 1) { boons += (boons.empty() ? "" : ", ") + std::string(Analysis::kBoonNames[b]); } }
			auto name = f.SkillNames.count(g.Skill) ? f.SkillNames.at(g.Skill) : std::to_string(g.Skill);
			std::printf("learned\t%s\tprof %u\t%d\t%s\t%s\n", g.Account.c_str(), g.Profession, g.Skill, name.c_str(), boons.c_str());
		}
		{
			// How well our spikes line up with enemy downs: downs inside a spike window, spikes with a down
			int inside = 0, productive = 0;
			for (int32_t d : f.EnemyDownMs)
			{
				for (int64_t s : f.OurSpikesMs) { if (d >= s - 1000 && d <= s + 4000) { inside++; break; } }
			}
			for (int64_t s : f.OurSpikesMs)
			{
				for (int32_t d : f.EnemyDownMs) { if (d >= s - 1000 && d <= s + 4000) { productive++; break; } }
			}
			std::printf("spikeeval\t%zu\t%d\t%zu\t%d\t%lld\n", f.OurSpikesMs.size(), productive, f.EnemyDownMs.size(), inside, static_cast<long long>(f.DurationMs));
		}
		std::printf("commander\t%s\n", f.Commander >= 0 ? f.Players[f.Commander].Name.c_str() : "-");
		for (const auto& p : f.Players)
		{
			std::printf("taken\t%s\tsg %d\thits in %zu\theals in %zu\tstrips in %zu\tstab stripped %d used up %d given lost %d\thp %zu\tpos %zu\tstab spans %zu\n",
				p.Name.c_str(), p.Subgroup, p.HitsIn.size(), p.HealsIn.size(), p.StripsIn.size(), p.StabStripped, p.StabUsedUp,
				p.StabGivenLost, p.Hp.size(), p.Pos.size(), p.BoonOn[Analysis::kStability].size());
		}
		std::printf("downs\t%zu of %d\t%zu of %d\n", f.SquadDownMs.size(), f.SquadDowns, f.EnemyDownMs.size(), f.EnemyDowns);
		for (const auto& p : f.Players)
		{
			if (p.Reviving.empty() && p.ReviveUses.empty()) { continue; }
			std::printf("revive\t%s\t%s\treviving %lld ms in %zu\tskills", p.Name.c_str(), p.Spec.c_str(), static_cast<long long>(p.ReviveMs), p.Reviving.size());
			for (auto& u : p.ReviveUses) { std::printf(" %d@%d:%s%d down,%d up", u.Skill, u.Ms, u.Done ? "" : "cut short,", u.DownNear, u.GotUp); }
			std::printf("\n");
		}
		for (auto& [g, n] : f.GroupCcWindows)
		{
			std::printf("groupcc\t%d\t%d of %d\n", g, f.GroupCcCovered.count(g) ? f.GroupCcCovered.at(g) : 0, n);
		}
		for (const auto& p : f.Players)
		{
			long long dmg = 0, heal = 0;
			for (int32_t v : p.DamagePerS) { dmg += v; }
			for (int32_t v : p.HealPerS) { heal += v; }
			size_t castTimes = 0;
			for (auto& [skill, row] : p.Skills) { castTimes += row.CastMs.size(); }
			std::printf("timeline\t%s\tcc %zu (%d no stab)\tspans %zu\tdowned %lld ms\tstab spans %zu\tdmg/s sum %lld of %lld\theal/s sum %lld of %lld\tcast times %zu\n",
				p.Name.c_str(), p.CcMs.size(), p.CcNoStab, p.DownSpans.size(), static_cast<long long>(p.DownedMs), p.StabOnMe.size(),
				dmg, static_cast<long long>(p.DamageAll), heal, static_cast<long long>(p.Heal), castTimes);
		}
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
