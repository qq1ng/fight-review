// Prints the C++ analysis of one log as tab-separated lines, for tools/check_cpp.py to compare with the Python
// prototype. Usage: frcheck <file.zevtc>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <exception>
#include <fstream>

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

// --keyskills: which of our skills matter to spikes, over many rounds. Per spec and skill:
//  offence (our spikes): damage to players within 1.5 s of our spike peaks, its share against the share of time those
//    windows take (lift), how often a cast had another player's cast of it within 1 s (synced), and enemy downs in
//    our spikes where it landed against where it didn't;
//  defence (enemy spikes): share of casts from 3 s before an enemy peak to 1 s after, against time (lift), and our
//    downs in enemy spikes where it was cast in the 3 s before the peak against where it wasn't.
int KeySkills(int argc, char** argv)
{
	struct Off { double Damage = 0, InSpike = 0; int Casts = 0, Synced = 0; double SyncChance = 0, DownsWith = 0; int SpikesWith = 0; };
	struct Def { int Casts = 0, InWindow = 0, Before = 0, After = 0; double DownsWith = 0; int SpikesWith = 0; double Stab = 0, Aegis = 0, Prot = 0, Resist = 0, Heal = 0, Cleanses = 0; };
	std::map<std::pair<std::string, std::string>, Off> off; // (spec, skill)
	std::map<std::pair<std::string, std::string>, Def> def;
	double fightMs = 0, ourWindowMs = 0, theirWindowMs = 0, ourSpikeDowns = 0, theirSpikeDowns = 0;
	int ourSpikes = 0, theirSpikes = 0, rounds = 0;
	// logs, or @file: one log path per line (a month of logs is too long a command line)
	std::vector<std::string> paths;
	for (int i = 2; i < argc; i++)
	{
		if (argv[i][0] != '@') { paths.push_back(argv[i]); continue; }
		std::ifstream in(argv[i] + 1);
		for (std::string line; std::getline(in, line);) { if (!line.empty()) { paths.push_back(line); } }
	}
	for (const std::string& path : paths)
	{
		Analysis::Fight f;
		try { f = Analysis::Analyse(path); } catch (...) { continue; }
		if (f.DurationMs < 30000 || f.Players.size() < 10) { continue; }
		rounds++;
		fightMs += f.DurationMs;
		ourWindowMs += 3000.0 * f.OurSpikesMs.size();
		theirWindowMs += 4000.0 * f.TheirSpikesMs.size();
		auto name = [&](int32_t sk) { auto it = f.SkillNames.find(sk); return it == f.SkillNames.end() ? std::to_string(sk) : it->second; };
		auto inOurs = [&](int64_t ms) { for (int64_t t : f.OurSpikesMs) { if (std::llabs(ms - t) <= 1500) { return true; } } return false; };
		// per spike: which skills landed (ours) or were cast in the build-up (theirs), and the downs in it
		std::vector<std::set<std::pair<std::string, std::string>>> ourHad(f.OurSpikesMs.size()), theirHad(f.TheirSpikesMs.size());
		std::vector<int> enemyDowns(f.OurSpikesMs.size()), ourDowns(f.TheirSpikesMs.size());
		for (size_t k = 0; k < f.OurSpikesMs.size(); k++) { for (int32_t d : f.EnemyDownMs) { enemyDowns[k] += d >= f.OurSpikesMs[k] - 3000 && d <= f.OurSpikesMs[k] + 4000; } }
		for (size_t k = 0; k < f.TheirSpikesMs.size(); k++) { for (int32_t d : f.SquadDownMs) { ourDowns[k] += d >= f.TheirSpikesMs[k] - 3000 && d <= f.TheirSpikesMs[k] + 4000; } }
		// every cast of a skill by anyone, for "synced"
		std::map<int32_t, std::vector<std::pair<int32_t, const Analysis::Player*>>> castsOf;
		for (const auto& p : f.Players) { for (auto& [sk, row] : p.Skills) { if (sk > 0) { for (int32_t c : row.CastMs) { castsOf[sk].push_back({c, &p}); } } } }
		for (const auto& p : f.Players)
		{
			for (const auto& h : p.HitsOut)
			{
				if (h.Skill <= 0) { continue; }
				Off& o = off[{p.Spec, name(h.Skill)}];
				o.Damage += h.Damage;
				if (inOurs(h.Ms)) { o.InSpike += h.Damage; }
				for (size_t k = 0; k < f.OurSpikesMs.size(); k++) { if (std::llabs(h.Ms - f.OurSpikesMs[k]) <= 1500) { ourHad[k].insert({p.Spec, name(h.Skill)}); } }
			}
			for (auto& [sk, row] : p.Skills)
			{
				if (sk <= 0 || row.CastMs.empty()) { continue; }
				auto key = std::make_pair(p.Spec, name(sk));
				Off& o = off[key];
				Def& d = def[key];
				for (int32_t c : row.CastMs)
				{
					o.Casts++;
					o.Synced += std::any_of(castsOf[sk].begin(), castsOf[sk].end(), [&](const auto& x) { return x.second != &p && std::abs(x.first - c) <= 1000; });
					// by chance: the others' casts of it spread evenly over the round, one within 1 s either side
					double others = static_cast<double>(castsOf[sk].size() - row.CastMs.size());
					o.SyncChance += 1.0 - std::exp(-others * 2000.0 / std::max<int64_t>(1, f.DurationMs));
					d.Casts++;
					bool in = false;
					for (size_t k = 0; k < f.TheirSpikesMs.size(); k++)
					{
						if (c >= f.TheirSpikesMs[k] - 3000 && c <= f.TheirSpikesMs[k] + 1000) { in = true; }
						if (c >= f.TheirSpikesMs[k] - 3000 && c < f.TheirSpikesMs[k]) { d.Before++; }
						if (c >= f.TheirSpikesMs[k] && c <= f.TheirSpikesMs[k] + 2000) { d.After++; }
						if (c >= f.TheirSpikesMs[k] - 3000 && c <= f.TheirSpikesMs[k]) { theirHad[k].insert(key); }
					}
					d.InWindow += in;
				}
				d.Stab += row.BoonSquadS[Analysis::kStability]; d.Aegis += row.BoonSquadS[7]; d.Prot += row.BoonSquadS[4]; d.Resist += row.BoonSquadS[10];
				d.Heal += static_cast<double>(row.Heal + row.Barrier); d.Cleanses += row.Cleanses;
			}
		}
		for (size_t k = 0; k < ourHad.size(); k++)
		{
			ourSpikes++; ourSpikeDowns += enemyDowns[k];
			for (const auto& key : ourHad[k]) { off[key].SpikesWith++; off[key].DownsWith += enemyDowns[k]; }
		}
		for (size_t k = 0; k < theirHad.size(); k++)
		{
			theirSpikes++; theirSpikeDowns += ourDowns[k];
			for (const auto& key : theirHad[k]) { def[key].SpikesWith++; def[key].DownsWith += ourDowns[k]; }
		}
	}
	const double ourShare = ourWindowMs / std::max(1.0, fightMs), theirShare = theirWindowMs / std::max(1.0, fightMs);
	std::printf("%d rounds (30 s+, 10+ of ours), %d of our spikes (%.0f%% of the time within 1.5 s of one), %d enemy spikes\n", rounds, ourSpikes, 100 * ourShare, theirSpikes);
	std::printf("enemy downs per our spike %.2f; our downs per enemy spike %.2f\n\n", ourSpikeDowns / std::max(1, ourSpikes), theirSpikeDowns / std::max(1, theirSpikes));
	std::printf("OFFENCE\tspec\tskill\tdamage\tin spikes\tshare in spikes\tlift\tcasts\tsynced\tsynced by chance\tspikes with it\tenemy downs with / without\n");
	std::vector<std::pair<double, std::pair<std::string, std::string>>> order;
	for (auto& [k, o] : off) { order.push_back({o.InSpike, k}); }
	std::sort(order.rbegin(), order.rend());
	for (size_t i = 0; i < order.size() && i < 400; i++)
	{
		const Off& o = off[order[i].second];
		double share = o.Damage > 0 ? o.InSpike / o.Damage : 0;
		double without = ourSpikes - o.SpikesWith > 0 ? (ourSpikeDowns - o.DownsWith) / (ourSpikes - o.SpikesWith) : 0;
		std::printf("off\t%s\t%s\t%.0f\t%.0f\t%.0f%%\t%.2f\t%d\t%.0f%%\t%.0f%%\t%d\t%.2f / %.2f\n", order[i].second.first.c_str(), order[i].second.second.c_str(), o.Damage, o.InSpike, 100 * share,
			share / std::max(1e-9, ourShare), o.Casts, o.Casts ? 100.0 * o.Synced / o.Casts : 0.0, o.Casts ? 100.0 * o.SyncChance / o.Casts : 0.0, o.SpikesWith, o.SpikesWith ? o.DownsWith / o.SpikesWith : 0.0, without);
	}
	std::printf("\nDEFENCE\tspec\tskill\tcasts\tin the 3 s before a peak\tlift\t2 s after\tgives\tspikes with it\tour downs with / without\n");
	std::vector<std::pair<double, std::pair<std::string, std::string>>> dorder;
	const double beforeShare = 3000.0 * theirSpikes / std::max(1.0, fightMs);
	for (auto& [k, d] : def)
	{
		bool protects = d.Stab > 50 || d.Aegis > 20 || d.Prot > 50 || d.Resist > 50 || d.Heal > 200000 || d.Cleanses > 50 ||
			k.second.find("Distortion") != std::string::npos || k.second.find("August Queen") != std::string::npos;
		if (d.Casts >= 60 && protects) { dorder.push_back({double(d.Before), k}); }
	}
	std::sort(dorder.rbegin(), dorder.rend());
	for (size_t i = 0; i < dorder.size() && i < 400; i++)
	{
		const Def& d = def[dorder[i].second];
		double without = theirSpikes - d.SpikesWith > 0 ? (theirSpikeDowns - d.DownsWith) / (theirSpikes - d.SpikesWith) : 0;
		std::string gives;
		if (d.Stab > 50) { gives += " stab"; }
		if (d.Aegis > 20) { gives += " aegis"; }
		if (d.Prot > 50) { gives += " prot"; }
		if (d.Resist > 50) { gives += " resist"; }
		if (d.Heal > 200000) { gives += " heal"; }
		if (d.Cleanses > 50) { gives += " cleanse"; }
		std::printf("def\t%s\t%s\t%d\t%.0f%%\t%.2f\t%.0f%%\t%s\t%d\t%.2f / %.2f\n", dorder[i].second.first.c_str(), dorder[i].second.second.c_str(), d.Casts, 100.0 * d.Before / d.Casts,
			double(d.Before) / d.Casts / std::max(1e-9, beforeShare), 100.0 * d.After / d.Casts, gives.c_str(), d.SpikesWith, d.SpikesWith ? d.DownsWith / d.SpikesWith : 0.0, without);
	}
	return 0;
}

// --revives @file: per spec and skill, over many rounds: revive skill uses (completed), with an ally down in reach,
// and allies up within 3 s; and healing on downed allies (Healing Stats) per skill, with the get-ups it was the
// biggest heal for (within 0.4 s of the get-up)
int Revives(int argc, char** argv)
{
	struct Row { int Uses = 0, WithDown = 0, GotUp = 0; double DownHeal = 0; int GetUps = 0; };
	std::map<std::pair<std::string, std::string>, Row> by;
	std::vector<std::string> paths;
	for (int i = 2; i < argc; i++)
	{
		if (argv[i][0] != '@') { paths.push_back(argv[i]); continue; }
		std::ifstream in(argv[i] + 1);
		for (std::string line; std::getline(in, line);) { if (!line.empty()) { paths.push_back(line); } }
	}
	int rounds = 0, getUps = 0;
	for (const std::string& path : paths)
	{
		Analysis::Fight f;
		try { f = Analysis::Analyse(path); } catch (...) { continue; }
		rounds++;
		auto name = [&](int32_t sk) { auto it = f.SkillNames.find(sk); return it == f.SkillNames.end() ? std::to_string(sk) : it->second; };
		for (const auto& p : f.Players)
		{
			for (const auto& u : p.ReviveUses)
			{
				if (!u.Done) { continue; }
				Row& r = by[{p.Spec, name(u.Skill)}];
				r.Uses++; r.WithDown += u.DownNear > 0; r.GotUp += u.GotUp;
			}
			for (const auto& h : p.DownHealsIn) { if (h.By >= 0) { by[{f.Players[h.By].Spec, name(h.Skill)}].DownHeal += h.Amount; } }
			for (size_t i = 0; i < p.DownSpans.size(); i++)
			{
				const auto& s = p.DownSpans[i];
				bool died = s.Dead || (i + 1 < p.DownSpans.size() && p.DownSpans[i + 1].Dead && p.DownSpans[i + 1].From == s.To);
				if (died || s.Dead) { continue; }
				getUps++;
				const Analysis::Player::DownHeal* best = nullptr;
				for (const auto& h : p.DownHealsIn) { if (h.By >= 0 && std::abs(h.Ms - s.To) <= 400 && (!best || h.Amount > best->Amount)) { best = &h; } }
				if (best) { by[{f.Players[best->By].Spec, name(best->Skill)}].GetUps++; }
			}
		}
	}
	std::printf("%d rounds, %d get-ups (not deaths)\n", rounds, getUps);
	std::printf("spec\tskill\trevive uses\twith a down in reach\tallies up within 3 s\tdowned healing\tget-ups it was the biggest heal for\n");
	std::vector<std::pair<double, std::pair<std::string, std::string>>> order;
	for (auto& [k, r] : by) { if (r.Uses >= 5 || r.DownHeal >= 1 || r.GetUps >= 1) { order.push_back({r.DownHeal + 1e9 * r.Uses, k}); } }
	std::sort(order.rbegin(), order.rend());
	for (auto& [v, k] : order)
	{
		const Row& r = by[k];
		std::printf("%s\t%s\t%d\t%d\t%d\t%.0f\t%d\n", k.first.c_str(), k.second.c_str(), r.Uses, r.WithDown, r.GotUp, r.DownHeal, r.GetUps);
	}
	return 0;
}

// --standard @file: Battle Standard (14419) cast near downed enemies: per cast, enemies downed within reach of the
// caster then, and how many of them died within 4 s; against downed enemies near the caster at any other moment
int Standard(int argc, char** argv)
{
	std::vector<std::string> paths;
	for (int i = 2; i < argc; i++)
	{
		if (argv[i][0] != '@') { paths.push_back(argv[i]); continue; }
		std::ifstream in(argv[i] + 1);
		for (std::string line; std::getline(in, line);) { if (!line.empty()) { paths.push_back(line); } }
	}
	std::map<std::string, std::array<int, 4>> by; // spec -> casts, with a downed enemy in reach, enemies downed in reach, of them died within 4 s
	int baseDown = 0, baseDied = 0; // every enemy down: died (anyone's finish)
	for (const std::string& path : paths)
	{
		Analysis::Fight f;
		try { f = Analysis::Analyse(path); } catch (...) { continue; }
		auto at = [](const std::vector<Analysis::Player::Point>& aPos, int32_t ms) -> const Analysis::Player::Point*
		{
			const Analysis::Player::Point* best = nullptr;
			for (const auto& p : aPos) { if (!best || std::abs(p.Ms - ms) < std::abs(best->Ms - ms)) { best = &p; } }
			return best && std::abs(best->Ms - ms) <= 2000 ? best : nullptr;
		};
		for (const auto& en : f.Enemies)
		{
			for (size_t i = 0; i < en.DownSpans.size(); i++)
			{
				if (en.DownSpans[i].Dead) { continue; }
				baseDown++;
				baseDied += i + 1 < en.DownSpans.size() && en.DownSpans[i + 1].Dead && en.DownSpans[i + 1].From == en.DownSpans[i].To;
			}
		}
		for (const auto& p : f.Players)
		{
			auto it = p.Skills.find(14419);
			if (it == p.Skills.end()) { continue; }
			for (int32_t c : it->second.CastMs)
			{
				auto& row = by[p.Spec];
				row[0]++;
				const auto* me = at(p.Pos, c);
				if (!me) { continue; }
				int near = 0, died = 0;
				for (const auto& en : f.Enemies)
				{
					for (size_t i = 0; i < en.DownSpans.size(); i++)
					{
						const auto& s = en.DownSpans[i];
						if (s.Dead || s.From > c + 2000 || s.To < c) { continue; } // downed at the cast, or within the 2 s cast
						const auto* them = at(en.Pos, c);
						if (!them || std::hypot(me->X - them->X, me->Y - them->Y) > 1200) { continue; }
						near++;
						died += i + 1 < en.DownSpans.size() && en.DownSpans[i + 1].Dead && en.DownSpans[i + 1].From == s.To && s.To <= c + 4000;
					}
				}
				row[1] += near > 0; row[2] += near; row[3] += died;
			}
		}
	}
	std::printf("enemy downs that died (anyone's finish): %d of %d (%.0f%%)\n", baseDied, baseDown, 100.0 * baseDied / std::max(1, baseDown));
	for (auto& [spec, r] : by)
	{
		std::printf("%s\tcasts %d\twith a downed enemy within 1200 %d\tdowned enemies in reach %d\tof them died within 4 s %d\n", spec.c_str(), r[0], r[1], r[2], r[3]);
	}
	return 0;
}

int main(int argc, char** argv)
{
	if (argc >= 3 && std::string(argv[1]) == "--standard") { return Standard(argc, argv); }
	if (argc >= 3 && std::string(argv[1]) == "--revives") { return Revives(argc, argv); }
	if (argc >= 3 && std::string(argv[1]) == "--keyskills") { return KeySkills(argc, argv); }
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
