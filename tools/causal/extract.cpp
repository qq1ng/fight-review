// The data behind the "why" view (src/Causes.*, src/UiStruggle.cpp): every log read, grouped into nights (a new night after
// a 3 h gap), each round's causes worked out with its night (Causes::Compute, with the risk sets), and written as CSV
// tables, numbers only (players as round-local indices plus a hash of the account, never names):
//   rounds.csv, players.csv, enemies.csv, ally_risk.csv, ally_downs.csv, enemy_risk.csv, enemy_downs.csv, pushes.csv,
//   summary.csv (Causes::Summarize: what the view shows, per round)
// tools/causal/analyse.py turns them into the odds and won-fight values the view carries (717 logs, 3 Sept to
// 6 Oct, 17 s). Keep the output in data/ (gitignored): account hashes are pseudonyms, not anonymous.
// Usage: frcauses <outdir> <logs or folders...>   (or @list.txt with one path a line; "path<TAB>night" lines, as
// tools/import_logs.py writes them, keep their order and nights)
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Analysis.h"
#include "Causes.h"

using namespace Analysis;

namespace
{
	uint32_t Hash(const std::string& s)
	{
		uint32_t h = 2166136261u;
		for (unsigned char c : s) { h ^= c; h *= 16777619u; }
		return h;
	}
	std::string F(double v) { char b[32]; std::snprintf(b, sizeof b, "%.4g", v); return b; }
	// a CSV field, quoted, quotes doubled (the log's path: whose logs a round came from)
	std::string Csv(const std::u8string& v)
	{
		std::string out = "\"";
		for (char8_t c : v) { if (c == u8'"') { out += '"'; } out += static_cast<char>(c); }
		return out + "\"";
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) { std::fprintf(stderr, "usage: extract <outdir> <logs...>\n"); return 1; }
	const std::string out = argv[1];
	std::vector<std::filesystem::path> paths;
	std::vector<int> given; // a list's own nights ("path<TAB>night", tools/import_logs.py: in server time, duplicates gone)
	for (int i = 2; i < argc; i++)
	{
		std::string a = argv[i];
		if (!a.empty() && a[0] == '@')
		{
			std::ifstream in(a.substr(1));
			for (std::string line; std::getline(in, line);)
			{
				if (!line.empty() && line.back() == '\r') { line.pop_back(); }
				if (line.empty()) { continue; }
				const size_t tab = line.find('\t');
				const std::string p = line.substr(0, tab); // UTF-8
				paths.push_back(std::filesystem::path(std::u8string(p.begin(), p.end())));
				if (tab != std::string::npos) { given.push_back(std::atoi(line.c_str() + tab + 1)); }
			}
		}
		else if (std::filesystem::is_directory(a))
		{
			// a folder: every log in it and below (another player's arcdps.cbtlogs folder as it comes)
			for (const auto& e : std::filesystem::recursive_directory_iterator(a)) { if (e.is_regular_file() && (e.path().extension() == ".zevtc" || e.path().extension() == ".evtc")) { paths.push_back(e.path()); } }
		}
		else { paths.push_back(a); }
	}
	std::vector<std::vector<std::filesystem::path>> nights;
	if (!given.empty() && given.size() == paths.size())
	{
		// the list's order and nights as given
		for (size_t i = 0; i < paths.size(); i++)
		{
			if (nights.empty() || given[i] != given[i - 1]) { nights.emplace_back(); }
			nights.back().push_back(paths[i]);
		}
	}
	else
	{
		std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
		// nights: by the stamp in the file name, a new one after 3 h
		auto stampOf = [](const std::filesystem::path& p) { Fight f; f.Stamp = p.stem().string(); return Causes::StampSeconds(f); };
		int64_t last = -1;
		for (const auto& p : paths)
		{
			const int64_t s = stampOf(p);
			if (nights.empty() || s - last > 3 * 3600) { nights.emplace_back(); }
			nights.back().push_back(p);
			last = s;
		}
	}

	std::ofstream summ(out + "/summary.csv");
	summ << "round,downs,deaths,w_cc,w_immob,w_noprot,w_kite,w_ranpast,w_hurt,c_cc,c_immob,c_noprot,c_kite,c_ranpast,c_hurt,ccstab,fast,ranout,unused,tooluses,onnobody,edowns,edeaths,apushes,pwithdowns,focus,cclanded,ccbefore,cleave,afterstop\n";
	std::ofstream rounds(out + "/rounds.csv"), players(out + "/players.csv"), enemies(out + "/enemies.csv"), ar(out + "/ally_risk.csv"),
		ad(out + "/ally_downs.csv"), er(out + "/enemy_risk.csv"), ed(out + "/enemy_downs.csv"), pu(out + "/pushes.csv");
	rounds << "round,night,stamp,server,map,dur,squad,enemies,adowns,adeaths,edowns,edeaths,aspikes,espikes,commander,pov,allystop,enemystop,file\n";
	players << "round,p,acct,spec,prof,sg,dmgplayer,active,damage,heal,barrier,stabally,cleanses,strips,commander,pov,downs,deaths\n";
	enemies << "round,e,spec,fought,team\n";
	ar << "ev,round,t,case,p,hits,enemies,dmg,cc,ccms,sincecc,stab,stabstrip,stabused,strips,corrupt,hardms,immobms,prot,aegis,resist,regen,quick,hp,healed,dodges,evaded,negated,stunbreaks,totag,tosquad,ahead,aheadbefore,movedin,sqmovedin,enmovedin,tele,pulled,alliesnear,enemiesnear,sgstab,inspike\n";
	ad << "ev,round,t,p,died,downms,cleave,reviving,byskill,alliesnear,enemiesnear,toolsready,toolsstrict,toolsspent,inspike,spike,rallied,afterstop\n";
	er << "ev,round,t,case,e,hits,allies,dmg,cc,blocked,stabstrip,strips,hardms,immobms,hp,invuln,totheir,ahead,alliesnear,enemiesnear,inspike\n";
	ed << "ev,round,t,e,died,downms,cleave,spike,rallied,endoflog,hitters,firsthit,cleaveearly,hittersearly,afterstop\n";
	// skills.csv: what each skill did in a round, per spec. Side a: allies' skills (Player::Skills; downs: enemy downs it made,
	// the last hit on the enemy in the 1 s before), side e: enemy skills on allies (Player::HitsIn; downs: ally downs it made).
	// skill_names.csv: the names the logs give them.
	std::ofstream sk(out + "/skills.csv");
	sk << "round,side,spec,skill,players,casts,hits,damage,strips,cleanses,heal,barrier,stab,boons,downs\n";
	std::map<int32_t, std::string> skillNames;
	pu << "round,ally,idx,first,last,peak,downs,deaths,up,hitting,dmgup,dmghitting,damage,topshare,targets,sync,focus,ccbefore,ccin,blocked,stabstrips,strips,immob,wells,negated,gap,hitallies,hitstab,hitstripped,hitccd\n";

	int roundId = 0, aev = 0, eev = 0;
	for (size_t n = 0; n < nights.size(); n++)
	{
		const auto& logs = nights[n];
		std::vector<std::unique_ptr<Fight>> fights(logs.size());
		std::atomic<size_t> next{0};
		std::vector<std::thread> pool;
		const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
		for (unsigned t = 0; t < threads; t++)
		{
			pool.emplace_back([&]
			{
				for (size_t i; (i = next++) < logs.size();)
				{
					try { fights[i] = std::make_unique<Fight>(Analyse(logs[i])); }
					catch (const std::exception& e) { std::fprintf(stderr, "%s: %s\n", logs[i].string().c_str(), e.what()); }
				}
			});
		}
		for (auto& t : pool) { t.join(); }
		Causes::Night night;
		for (const auto& f : fights) { if (f) { night.Rounds.push_back(f.get()); } }
		// the rounds' causes, in parallel
		std::vector<Causes::Round> causes(night.Rounds.size());
		next = 0;
		pool.clear();
		for (unsigned t = 0; t < threads; t++)
		{
			pool.emplace_back([&]
			{
				for (size_t i; (i = next++) < night.Rounds.size();)
				{
					Causes::Night nt = night;
					nt.Index = static_cast<int>(i);
					causes[i] = Causes::Compute(*night.Rounds[i], nt);
				}
			});
		}
		for (auto& t : pool) { t.join(); }

		for (size_t i = 0; i < night.Rounds.size(); i++)
		{
			const Fight& f = *night.Rounds[i];
			const Causes::Round& c = causes[i];
			const int r = roundId++;
			rounds << r << ',' << n << ',' << f.Stamp << ',' << f.ServerStart << ',' << f.MapId << ',' << f.DurationMs << ',' << f.SquadCount << ',' << f.EnemyCount << ',' << f.SquadDowns << ',' << f.SquadDeaths << ','
				<< f.EnemyDowns << ',' << f.EnemyDeaths << ',' << f.OurSpikesMs.size() << ',' << f.TheirSpikesMs.size() << ',' << f.Commander << ',' << f.Pov << ',' << c.AllyStopMs << ',' << c.EnemyStopMs << ',' << Csv(f.Path.u8string()) << '\n';
			std::vector<int64_t> dmg;
			for (const Player& p : f.Players) { if (p.Damage > 0) { dmg.push_back(p.Damage); } }
			std::sort(dmg.begin(), dmg.end());
			const int64_t floor = dmg.empty() ? 1 : dmg[dmg.size() / 2];
			for (size_t k = 0; k < f.Players.size(); k++)
			{
				const Player& p = f.Players[k];
				players << r << ',' << k << ',' << Hash(p.Account) << ',' << p.Spec << ',' << p.Profession << ',' << p.Subgroup << ',' << (p.Damage >= floor && p.Damage > 0) << ','
					<< p.ActiveMs << ',' << p.Damage << ',' << (p.HealKnown ? p.Heal : -1) << ',' << (p.HealKnown ? p.Barrier : -1) << ',' << p.StabAllyMs << ',' << p.Cleanses << ','
					<< p.Strips << ',' << (f.Commander == static_cast<int>(k)) << ',' << p.Pov << ',' << p.Downs << ',' << p.Deaths << '\n';
			}
			for (size_t k = 0; k < f.Enemies.size(); k++) { enemies << r << ',' << k << ',' << f.Enemies[k].Spec << ',' << f.Enemies[k].Fought << ',' << f.Enemies[k].Team << '\n'; }
			for (const auto& d : c.AllyDowns)
			{
				const int ev = aev++;
				ad << ev << ',' << r << ',' << d.Ms << ',' << d.Player << ',' << d.Died << ',' << d.DownMs << ',' << d.Cleave << ',' << d.Reviving << ',' << d.RevivedBySkill << ','
					<< d.AlliesNear << ',' << d.EnemiesNear << ',' << d.ToolsReady << ',' << d.ToolsReadyStrict << ',' << d.ToolsSpentBefore << ',' << d.InEnemySpike << ',' << d.Spike << ',' << d.Rallied << ',' << d.AfterStop << '\n';
				for (const auto& s : d.Risk)
				{
					ar << ev << ',' << r << ',' << d.Ms << ',' << s.Case << ',' << s.Player << ',' << s.Hits << ',' << s.Enemies << ',' << s.Damage << ',' << s.Cc << ',' << s.CcMs << ','
						<< s.SinceCc << ',' << s.StabAt << ',' << s.StabStripped << ',' << s.StabUsedUp << ',' << s.Strips << ',' << s.Corrupted << ',' << s.HardMs << ',' << s.ImmobMs << ','
						<< s.Protection << ',' << s.Aegis << ',' << s.Resistance << ',' << s.Regeneration << ',' << s.Quickness << ',' << s.HpBefore << ',' << s.Healed << ',' << s.Dodges << ','
						<< s.Evaded << ',' << s.Negated << ',' << s.StunBreaks << ',' << F(s.ToTag) << ',' << F(s.ToSquad) << ',' << F(s.Ahead) << ',' << F(s.AheadBefore) << ',' << F(s.MovedIn) << ',' << F(s.SquadMovedIn) << ',' << F(s.EnemyMovedIn) << ',' << s.Teleported << ',' << s.Pulled << ',' << s.AlliesNear << ','
						<< s.EnemiesNear << ',' << s.SubgroupStab << ',' << s.InEnemySpike << '\n';
				}
			}
			for (const auto& d : c.EnemyDowns)
			{
				const int ev = eev++;
				ed << ev << ',' << r << ',' << d.Ms << ',' << d.Enemy << ',' << d.Died << ',' << d.DownMs << ',' << d.Cleave << ',' << d.Spike << ',' << d.Rallied << ',' << d.EndOfLog << ',' << d.Hitters << ',' << d.FirstHit << ',' << d.CleaveEarly << ',' << d.HittersEarly << ',' << d.AfterStop << '\n';
				for (const auto& s : d.Risk)
				{
					er << ev << ',' << r << ',' << d.Ms << ',' << s.Case << ',' << s.Enemy << ',' << s.Hits << ',' << s.Allies << ',' << s.Damage << ',' << s.Cc << ',' << s.StabBlocked << ','
						<< s.StabStripped << ',' << s.Strips << ',' << s.HardMs << ',' << s.ImmobMs << ',' << s.HpBefore << ',' << s.Invulnerable << ',' << F(s.ToTheirSquad) << ','
						<< F(s.Ahead) << ',' << s.AlliesNear << ',' << s.EnemiesNear << ',' << s.InAllySpike << '\n';
				}
			}
			{
				const Causes::Summary s = Causes::Summarize(f, c);
				summ << r << ',' << s.Downs << ',' << s.Deaths;
				for (int k = 0; k < Causes::F_Count; k++) { summ << ',' << s.With[k]; }
				for (int k = 0; k < Causes::F_Count; k++) { summ << ',' << F(s.Cost[k]); }
				summ << ',' << s.CcStabStripped << ',' << s.DiedFast << ',' << s.DiedRanOut << ',' << s.DiedUnused << ',' << s.ToolUses << ',' << s.ToolsOnNobody << ',' << s.EnemyDowns << ','
					<< s.EnemyDeaths << ',' << s.AllyPushes << ',' << s.PushesWithDowns << ',' << F(s.Focus) << ',' << F(s.CcLanded) << ',' << F(s.CcBefore) << ',' << s.CleaveMedian << ',' << s.AfterStopDowns << '\n';
			}
			for (const auto& u : c.Pushes)
			{
				pu << r << ',' << u.Ally << ',' << u.Index << ',' << u.First << ',' << u.Last << ',' << u.Peak << ',' << u.Downs << ',' << u.Deaths << ',' << u.Up << ',' << u.Hitting << ','
					<< u.DamageUp << ',' << u.DamageHitting << ',' << u.Damage << ',' << F(u.TopTargetShare) << ',' << u.Targets << ',' << F(u.Sync) << ',' << F(u.Focus) << ',' << u.CcBefore << ',' << u.CcIn << ','
					<< u.Blocked << ',' << u.StabStripsBefore << ',' << u.StripsBefore << ',' << u.Immobilized << ',' << u.Wells << ',' << u.Negated << ',' << F(u.Gap) << ','
					<< u.HitAllies << ',' << u.HitWithStab << ',' << u.HitStripped << ',' << u.HitCcd << '\n';
			}
			{
				for (const auto& [id, name] : f.SkillNames) { skillNames.emplace(id, name); }
				// the hit that put each down in: the last one on them in the 1 s before
				struct H { int32_t Ms; int By; int32_t Skill; };
				std::vector<std::vector<H>> onEnemy(f.Enemies.size());
				for (size_t k = 0; k < f.Players.size(); k++)
				{
					for (const auto& h : f.Players[k].HitsOut) { if (h.Enemy >= 0 && h.Damage > 0) { onEnemy[h.Enemy].push_back({h.Ms, static_cast<int>(k), h.Skill}); } }
				}
				auto downer = [](std::vector<H>& hits, int32_t aMs) -> const H*
				{
					auto it = std::upper_bound(hits.begin(), hits.end(), aMs, [](int32_t ms, const H& h) { return ms < h.Ms; });
					if (it == hits.begin()) { return nullptr; }
					--it;
					return it->Ms >= aMs - 1000 ? &*it : nullptr;
				};
				std::map<std::pair<std::string, int32_t>, int> allyDowns, enemyDowns;
				for (size_t e = 0; e < f.Enemies.size(); e++)
				{
					std::stable_sort(onEnemy[e].begin(), onEnemy[e].end(), [](const H& a, const H& b) { return a.Ms < b.Ms; });
					for (const auto& s : f.Enemies[e].DownSpans)
					{
						if (s.Dead) { continue; }
						if (const H* h = downer(onEnemy[e], s.From)) { allyDowns[{f.Players[h->By].Spec, h->Skill}]++; }
					}
				}
				struct Agg { int Players = 0, Casts = 0, Hits = 0, Strips = 0, Cleanses = 0; int64_t Damage = 0, Heal = 0, Barrier = 0; double Stab = 0, Boons = 0; };
				std::map<std::pair<std::string, int32_t>, Agg> ally, foe;
				for (const Player& p : f.Players)
				{
					for (const auto& [id, s] : p.Skills)
					{
						Agg& a = ally[{p.Spec, id}];
						a.Players++; a.Casts += s.Casts; a.Hits += s.Hits; a.Damage += s.Damage; a.Strips += s.Strips; a.Cleanses += s.Cleanses;
						a.Heal += s.Heal; a.Barrier += s.Barrier; a.Stab += s.BoonSquadS[kStability];
						for (int b = 0; b < kBoons; b++) { a.Boons += s.BoonSquadS[b]; }
					}
					std::vector<H> in;
					for (const auto& h : p.HitsIn)
					{
						const std::string spec = h.Enemy >= 0 ? f.Enemies[h.Enemy].Spec : std::string("?");
						Agg& a = foe[{spec, h.Skill}];
						a.Hits++; a.Damage += h.Damage;
						if (h.Damage > 0) { in.push_back({h.Ms, h.Enemy, h.Skill}); }
					}
					std::stable_sort(in.begin(), in.end(), [](const H& a, const H& b) { return a.Ms < b.Ms; });
					for (const auto& s : p.DownSpans)
					{
						if (s.Dead) { continue; }
						if (const H* h = downer(in, s.From)) { enemyDowns[{h->By >= 0 ? f.Enemies[h->By].Spec : std::string("?"), h->Skill}]++; }
					}
				}
				for (const auto& [key, d] : allyDowns) { ally[key]; }
				for (const auto& [key, d] : enemyDowns) { foe[key]; }
				for (const auto& [key, a] : ally)
				{
					const auto d = allyDowns.find(key);
					sk << r << ",a," << key.first << ',' << key.second << ',' << a.Players << ',' << a.Casts << ',' << a.Hits << ',' << a.Damage << ',' << a.Strips << ',' << a.Cleanses << ','
						<< a.Heal << ',' << a.Barrier << ',' << F(a.Stab) << ',' << F(a.Boons) << ',' << (d == allyDowns.end() ? 0 : d->second) << '\n';
				}
				for (const auto& [key, a] : foe)
				{
					const auto d = enemyDowns.find(key);
					sk << r << ",e," << key.first << ',' << key.second << ",0,0," << a.Hits << ',' << a.Damage << ",0,0,0,0,0,0," << (d == enemyDowns.end() ? 0 : d->second) << '\n';
				}
			}
		}
		std::fprintf(stderr, "night %zu: %zu rounds\n", n, night.Rounds.size());
	}
	std::ofstream names(out + "/skill_names.csv");
	names << "skill,name\n";
	for (const auto& [id, name] : skillNames)
	{
		std::string q = name;
		for (size_t i = 0; (i = q.find('"', i)) != std::string::npos; i += 2) { q.insert(i, 1, '"'); }
		names << id << ",\"" << q << "\"\n";
	}
	return 0;
}
