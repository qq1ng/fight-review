// Prints the C++ analysis of one log as tab-separated lines, for tools/check_cpp.py to compare with the Python
// prototype. Usage: frcheck <file.zevtc>
#include <cstdio>
#include <exception>

#include "Analysis.h"

int main(int argc, char** argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: frcheck <file.zevtc>\n"); return 2; }
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
			std::printf("learned\tprof %u\t%d\t%s\t%s\n", g.Profession, g.Skill, name.c_str(), boons.c_str());
		}
		std::printf("downs\t%zu of %d\t%zu of %d\n", f.SquadDownMs.size(), f.SquadDowns, f.EnemyDownMs.size(), f.EnemyDowns);
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
