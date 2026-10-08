#include "Causes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace Causes
{
	using Analysis::kStability;
	namespace
	{
		constexpr int kQuickness = 2, kProtection = 4, kRegeneration = 5, kAegis = 7, kResistance = 10;

		// When a side stopped fighting for good (Round::AllyStopMs): its damage per second (Fight::ToPlayersPerS, InPerS)
		// summed over 5 s stays under 35% of its fighting level (the upper quartile of those sums) from there to the end, for
		// 5 s or more. Ms from fight start, -1 when it fought to the end. 35%: of the cut-offs tried (20-50%, 3-5 s sums), it
		// split lost rounds from won ones best (752 + 140 rounds: 22% and 18% of ally downs after it in lost rounds, 3% and
		// 5% in won ones).
		int32_t StopAt(const std::vector<int64_t>& aPerS)
		{
			const int n = static_cast<int>(aPerS.size());
			if (n < 10) { return -1; }
			std::vector<int64_t> sums(n, 0), fighting;
			for (int i = 0; i < n; i++)
			{
				for (int k = i; k < std::min(n, i + 5); k++) { sums[i] += aPerS[k]; }
				if (sums[i] > 0) { fighting.push_back(sums[i]); }
			}
			if (fighting.empty()) { return -1; }
			std::sort(fighting.begin(), fighting.end());
			const double quiet = 0.35 * static_cast<double>(fighting[fighting.size() * 3 / 4]);
			int t = n;
			while (t > 0 && static_cast<double>(sums[t - 1]) < quiet) { t--; }
			return n - t >= 5 ? t * 1000 : -1;
		}

		// the first element at or after aMs in a time-ordered vector, by a member giving its time
		template <class T, class Get>
		size_t From(const std::vector<T>& v, int32_t aMs, Get aGet)
		{
			return static_cast<size_t>(std::lower_bound(v.begin(), v.end(), aMs, [&](const T& x, int32_t ms) { return aGet(x) < ms; }) - v.begin());
		}

		const Player::Point* PosNear(const std::vector<Player::Point>& v, int32_t aMs, int32_t aMaxGap = 1000)
		{
			auto it = std::lower_bound(v.begin(), v.end(), aMs, [](const Player::Point& p, int32_t ms) { return p.Ms < ms; });
			const Player::Point* best = nullptr;
			if (it != v.end()) { best = &*it; }
			if (it != v.begin() && (!best || aMs - (it - 1)->Ms < best->Ms - aMs)) { best = &*(it - 1); }
			return best && std::abs(best->Ms - aMs) <= aMaxGap ? best : nullptr;
		}

		float Dist(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

		struct Pt { float X = 0, Y = 0; bool Ok = false; };

		bool EnemyUp(const Fight::Enemy& e, int32_t aMs)
		{
			for (const auto& s : e.DownSpans) { if (s.From <= aMs && s.To >= aMs) { return false; } }
			return true;
		}

		Pt Median(std::vector<float>& xs, std::vector<float>& ys)
		{
			Pt m;
			if (xs.empty()) { return m; }
			const size_t h = xs.size() / 2;
			std::nth_element(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(h), xs.end());
			std::nth_element(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(h), ys.end());
			m.X = xs[h]; m.Y = ys[h]; m.Ok = true;
			return m;
		}

		// Per round: lookups the states share (both sides' middles over time, the hits on each enemy)
		struct Ctx
		{
			const Fight& F;
			struct HitRef { int32_t Ms; int Player; int32_t Damage; };
			std::vector<std::vector<HitRef>> OnEnemy;      // ally strikes on each enemy, time order
			std::unordered_map<int32_t, Pt> SquadMids, EnemyMids; // by 250 ms bin
			std::vector<bool> DamagePlayer;                 // at least the median damage of those who did any
			std::vector<int32_t> Wells;

			explicit Ctx(const Fight& f) : F(f)
			{
				OnEnemy.resize(f.Enemies.size());
				for (size_t i = 0; i < f.Players.size(); i++)
				{
					for (const auto& h : f.Players[i].HitsOut)
					{
						if (h.Enemy >= 0 && static_cast<size_t>(h.Enemy) < OnEnemy.size() && h.Damage > 0) { OnEnemy[h.Enemy].push_back({h.Ms, static_cast<int>(i), h.Damage}); }
					}
				}
				for (auto& v : OnEnemy) { std::sort(v.begin(), v.end(), [](const HitRef& a, const HitRef& b) { return a.Ms < b.Ms; }); }
				std::vector<int64_t> dmg;
				for (const Player& p : f.Players) { if (p.Damage > 0) { dmg.push_back(p.Damage); } }
				std::sort(dmg.begin(), dmg.end());
				const int64_t floor = dmg.empty() ? 1 : dmg[dmg.size() / 2];
				for (const Player& p : f.Players) { DamagePlayer.push_back(p.Damage >= floor && p.Damage > 0); }
				for (const Player& p : f.Players)
				{
					for (auto& [sk, row] : p.Skills)
					{
						auto n = f.SkillNames.find(sk);
						if (n != f.SkillNames.end() && (n->second == "Well of Corruption" || n->second == "Well of Suffering")) { Wells.insert(Wells.end(), row.CastMs.begin(), row.CastMs.end()); }
					}
				}
				std::sort(Wells.begin(), Wells.end());
			}

			// The squad's middle: the median position of the allies up
			Pt SquadMid(int32_t aMs)
			{
				const int32_t bin = aMs / 250;
				if (auto it = SquadMids.find(bin); it != SquadMids.end()) { return it->second; }
				std::vector<float> xs, ys;
				for (const Player& p : F.Players)
				{
					if (!UpAt(p, aMs)) { continue; }
					if (const auto* pt = PosNear(p.Pos, aMs)) { xs.push_back(pt->X); ys.push_back(pt->Y); }
				}
				return SquadMids[bin] = Median(xs, ys);
			}
			// The enemy's middle: enemies up, within 3000 of the squad's middle (not a third side's fight far off)
			Pt EnemyMid(int32_t aMs)
			{
				const int32_t bin = aMs / 250;
				if (auto it = EnemyMids.find(bin); it != EnemyMids.end()) { return it->second; }
				const Pt sq = SquadMid(aMs);
				std::vector<float> xs, ys;
				for (const auto& e : F.Enemies)
				{
					if (!e.Fought || !EnemyUp(e, aMs)) { continue; }
					const auto* pt = PosNear(e.Pos, aMs);
					if (!pt || (sq.Ok && Dist(pt->X, pt->Y, sq.X, sq.Y) > 3000)) { continue; }
					xs.push_back(pt->X); ys.push_back(pt->Y);
				}
				return EnemyMids[bin] = Median(xs, ys);
			}
		};

		AllyState AllyAt(Ctx& c, int aPlayer, int32_t aMs)
		{
			const Fight& f = c.F;
			const Player& p = f.Players[aPlayer];
			AllyState s;
			s.Player = aPlayer;
			const int32_t from = aMs - kLookBack;
			std::set<int> enemies;
			for (size_t i = From(p.HitsIn, from, [](const Player::TakenHit& h) { return h.Ms; }); i < p.HitsIn.size() && p.HitsIn[i].Ms <= aMs; i++)
			{
				const auto& h = p.HitsIn[i];
				s.Hits++; s.Damage += h.Damage;
				if (h.Enemy >= 0) { enemies.insert(h.Enemy); }
			}
			s.Enemies = static_cast<int>(enemies.size());
			for (size_t i = From(p.CcIn, from, [](const Player::CcHit& h) { return h.Ms; }); i < p.CcIn.size() && p.CcIn[i].Ms <= aMs; i++)
			{
				const auto& h = p.CcIn[i];
				s.Cc++;
				s.CcMs += std::min(std::max(0, h.Duration), aMs - h.Ms);
				s.SinceCc = aMs - h.Ms;
				s.Pulled += h.Kind == Analysis::CC_Pull || h.Kind == Analysis::CC_Taunt || h.Kind == Analysis::CC_KnockbackOrPull;
			}
			s.StabAt = OnAt(p.BoonOn[kStability], aMs - 500);
			for (size_t i = From(p.StabLost, aMs - kStripLookBack, [](const std::pair<int32_t, int>& x) { return x.first; }); i < p.StabLost.size() && p.StabLost[i].first <= aMs; i++)
			{
				if (p.StabLost[i].second == 0) { s.StabStripped = true; } else { s.StabUsedUp++; }
			}
			for (size_t i = From(p.StripsIn, from, [](const Player::StripHit& h) { return h.Ms; }); i < p.StripsIn.size() && p.StripsIn[i].Ms <= aMs; i++)
			{
				s.Strips++; s.Corrupted += p.StripsIn[i].Corrupted;
			}
			s.HardMs = TimeIn(p.HardCondOn, from, aMs);
			s.ImmobMs = TimeIn(p.ImmobOn, from, aMs);
			s.Protection = OnAt(p.BoonOn[kProtection], aMs - 500);
			s.Aegis = OnAt(p.BoonOn[kAegis], aMs - 500);
			s.Resistance = OnAt(p.BoonOn[kResistance], aMs - 500);
			s.Regeneration = OnAt(p.BoonOn[kRegeneration], aMs - 500);
			s.Quickness = OnAt(p.BoonOn[kQuickness], aMs - 500);
			{
				// health when the window opened: the last reading before it, else the first in it
				size_t i = From(p.Hp, from, [](const std::pair<int32_t, int32_t>& x) { return x.first; });
				if (i > 0 && from - p.Hp[i - 1].first <= 5000) { s.HpBefore = p.Hp[i - 1].second; }
				else if (i < p.Hp.size() && p.Hp[i].first <= aMs) { s.HpBefore = p.Hp[i].second; }
			}
			for (size_t i = From(p.HealsIn, from, [](const std::pair<int32_t, int32_t>& x) { return x.first; }); i < p.HealsIn.size() && p.HealsIn[i].first <= aMs; i++) { s.Healed += p.HealsIn[i].second; }
			for (size_t i = From(p.DodgeMs, from, [](int32_t x) { return x; }); i < p.DodgeMs.size() && p.DodgeMs[i] <= aMs; i++) { s.Dodges++; }
			for (size_t i = From(p.EvadedIn, from, [](const Player::TakenHit& h) { return h.Ms; }); i < p.EvadedIn.size() && p.EvadedIn[i].Ms <= aMs; i++) { s.Evaded++; }
			for (size_t i = From(f.NegatedHits, from, [](const Fight::Negated& n) { return n.Ms; }); i < f.NegatedHits.size() && f.NegatedHits[i].Ms <= aMs; i++)
			{
				s.Negated += f.NegatedHits[i].Player == aPlayer && f.NegatedHits[i].Kind != 4;
			}
			for (size_t i = From(f.StunBreaks, from, [](const Fight::StunBreak& b) { return b.Ms; }); i < f.StunBreaks.size() && f.StunBreaks[i].Ms <= aMs; i++)
			{
				s.StunBreaks += f.StunBreaks[i].Player == aPlayer;
			}
			// where they stood
			if (const auto* me = PosNear(p.Pos, aMs))
			{
				if (f.Commander >= 0 && f.Commander != aPlayer)
				{
					if (const auto* tag = PosNear(f.Players[f.Commander].Pos, aMs)) { s.ToTag = Dist(me->X, me->Y, tag->X, tag->Y); }
				}
				const Pt sq = c.SquadMid(aMs), en = c.EnemyMid(aMs);
				if (sq.Ok) { s.ToSquad = Dist(me->X, me->Y, sq.X, sq.Y); }
				if (sq.Ok && en.Ok) { s.Ahead = Dist(sq.X, sq.Y, en.X, en.Y) - Dist(me->X, me->Y, en.X, en.Y); }
				if (const auto* was = PosNear(p.Pos, from))
				{
					const Pt sq0 = c.SquadMid(from), en0 = c.EnemyMid(from);
					if (sq0.Ok && en0.Ok)
					{
						s.AheadBefore = Dist(sq0.X, sq0.Y, en0.X, en0.Y) - Dist(was->X, was->Y, en0.X, en0.Y);
						// a move (dx, dy) along the line from a to b
						auto along = [](float ax, float ay, float bx, float by, float dx, float dy)
						{
							const float l = Dist(ax, ay, bx, by);
							return l > 1 ? ((bx - ax) * dx + (by - ay) * dy) / l : 0.f;
						};
						s.MovedIn = along(was->X, was->Y, en0.X, en0.Y, me->X - was->X, me->Y - was->Y);
						if (sq.Ok) { s.SquadMovedIn = along(sq0.X, sq0.Y, en0.X, en0.Y, sq.X - sq0.X, sq.Y - sq0.Y); }
						if (en.Ok) { s.EnemyMovedIn = along(en0.X, en0.Y, was->X, was->Y, en.X - en0.X, en.Y - en0.Y); }
					}
				}
				for (int32_t t : p.TeleportMs) { if (t >= from && t <= aMs) { s.Teleported = true; break; } }
				for (size_t j = 0; j < f.Players.size(); j++)
				{
					if (static_cast<int>(j) == aPlayer || !UpAt(f.Players[j], aMs)) { continue; }
					if (const auto* q = PosNear(f.Players[j].Pos, aMs); q && Dist(me->X, me->Y, q->X, q->Y) <= kNear) { s.AlliesNear++; }
				}
				for (const auto& e : f.Enemies)
				{
					if (!EnemyUp(e, aMs)) { continue; }
					if (const auto* q = PosNear(e.Pos, aMs); q && Dist(me->X, me->Y, q->X, q->Y) <= kNear) { s.EnemiesNear++; }
				}
			}
			int sg = 0, sgStab = 0;
			for (size_t j = 0; j < f.Players.size(); j++)
			{
				const Player& q = f.Players[j];
				if (static_cast<int>(j) == aPlayer || q.Subgroup != p.Subgroup || !UpAt(q, aMs - 500)) { continue; }
				sg++;
				sgStab += OnAt(q.BoonOn[kStability], aMs - 500);
			}
			if (sg) { s.SubgroupStab = 100 * sgStab / sg; }
			s.InEnemySpike = SpikeAt(f, false, aMs) >= 0;
			return s;
		}

		EnemyState EnemyAt(Ctx& c, int aEnemy, int32_t aMs)
		{
			const Fight& f = c.F;
			const Fight::Enemy& e = f.Enemies[aEnemy];
			EnemyState s;
			s.Enemy = aEnemy;
			const int32_t from = aMs - kLookBack;
			std::set<int> allies;
			const auto& on = c.OnEnemy[aEnemy];
			for (size_t i = From(on, from, [](const Ctx::HitRef& h) { return h.Ms; }); i < on.size() && on[i].Ms <= aMs; i++)
			{
				s.Hits++; s.Damage += on[i].Damage; allies.insert(on[i].Player);
			}
			s.Allies = static_cast<int>(allies.size());
			auto count = [&](const std::vector<Fight::Enemy::Touch>& v, int32_t aFrom, int aBoon = -2)
			{
				int n = 0;
				for (size_t i = From(v, aFrom, [](const Fight::Enemy::Touch& t) { return t.Ms; }); i < v.size() && v[i].Ms <= aMs; i++) { n += aBoon == -2 || v[i].Boon == aBoon; }
				return n;
			};
			s.Cc = count(e.CcIn, from);
			s.StabBlocked = count(e.StabBlocked, from);
			s.StabStripped = count(e.StripsIn, aMs - kStripLookBack, kStability) > 0;
			s.Strips = count(e.StripsIn, from);
			s.HardMs = TimeIn(e.HardCondOn, from, aMs);
			s.ImmobMs = TimeIn(e.ImmobOn, from, aMs);
			{
				size_t i = From(e.Hp, from, [](const std::pair<int32_t, int32_t>& x) { return x.first; });
				if (i > 0 && from - e.Hp[i - 1].first <= 5000) { s.HpBefore = e.Hp[i - 1].second; }
				else if (i < e.Hp.size() && e.Hp[i].first <= aMs) { s.HpBefore = e.Hp[i].second; }
			}
			// up to 1.2 s before the moment: going down comes with a moment of invulnerability of its own
			for (int32_t sec = std::max(0, from / 1000); sec <= (aMs - 1200) / 1000 && sec < static_cast<int32_t>(f.InvulnTheirs.size()); sec++)
			{
				const auto& v = f.InvulnTheirs[sec];
				s.Invulnerable += std::find(v.begin(), v.end(), aEnemy) != v.end();
			}
			if (const auto* me = PosNear(e.Pos, aMs))
			{
				const Pt sq = c.SquadMid(aMs), en = c.EnemyMid(aMs);
				if (en.Ok) { s.ToTheirSquad = Dist(me->X, me->Y, en.X, en.Y); }
				if (sq.Ok && en.Ok) { s.Ahead = Dist(en.X, en.Y, sq.X, sq.Y) - Dist(me->X, me->Y, sq.X, sq.Y); }
				for (const Player& q : f.Players)
				{
					if (!UpAt(q, aMs)) { continue; }
					if (const auto* pt = PosNear(q.Pos, aMs); pt && Dist(me->X, me->Y, pt->X, pt->Y) <= kNear) { s.AlliesNear++; }
				}
				for (size_t j = 0; j < f.Enemies.size(); j++)
				{
					if (static_cast<int>(j) == aEnemy || !EnemyUp(f.Enemies[j], aMs)) { continue; }
					if (const auto* pt = PosNear(f.Enemies[j].Pos, aMs); pt && Dist(me->X, me->Y, pt->X, pt->Y) <= kNear) { s.EnemiesNear++; }
				}
			}
			s.InAllySpike = SpikeAt(f, true, aMs) >= 0;
			return s;
		}

		// Focus fire: for hits on one target in time order ((ms, attacker), attacker -1 = not counted), which of those in
		// [aFrom, aTo] had 10+ distinct attackers within 1.5 s either side (a sliding window, one pass)
		std::vector<bool> Focused(const std::vector<std::pair<int32_t, int>>& aHits, int32_t aFrom, int32_t aTo, size_t aAttackers)
		{
			std::vector<bool> out(aHits.size(), false);
			std::vector<int> count(aAttackers + 1, 0);
			int distinct = 0;
			size_t lo = 0, hi = 0;
			for (size_t h = 0; h < aHits.size(); h++)
			{
				const int32_t ms = aHits[h].first;
				while (hi < aHits.size() && aHits[hi].first <= ms + 1500)
				{
					const int a = aHits[hi].second;
					if (a >= 0 && static_cast<size_t>(a) < count.size() && count[a]++ == 0) { distinct++; }
					hi++;
				}
				while (lo < hi && aHits[lo].first < ms - 1500)
				{
					const int a = aHits[lo].second;
					if (a >= 0 && static_cast<size_t>(a) < count.size() && --count[a] == 0) { distinct--; }
					lo++;
				}
				out[h] = ms >= aFrom && ms <= aTo && aHits[h].second >= 0 && distinct >= 10;
			}
			return out;
		}

		// A spike run's first and last seconds' middles (the addon's SpikeWindow with no margins)
		std::pair<int32_t, int32_t> RunOf(const Fight& f, bool aOurs, size_t aIndex)
		{
			const auto& peaks = aOurs ? f.OurSpikesMs : f.TheirSpikesMs;
			const auto& spans = aOurs ? f.OurSpikeSpans : f.TheirSpikeSpans;
			const int32_t p = static_cast<int32_t>(peaks[aIndex]);
			if (aIndex >= spans.size()) { return {p, p}; }
			return {std::min(p, spans[aIndex].first + 500), std::max(p, spans[aIndex].second - 500)};
		}

		// When each revive tool was last used, across the night, as seconds since 2000 (from the logs' names)
		struct ToolUse { std::string Account; int64_t At; int32_t Skill; };
	}

	bool UpAt(const Player& p, int32_t aMs)
	{
		for (const auto& s : p.DownSpans) { if (s.From <= aMs && s.To >= aMs) { return false; } }
		return true;
	}

	bool OnAt(const std::vector<std::pair<int32_t, int32_t>>& aSpans, int32_t aMs)
	{
		auto it = std::upper_bound(aSpans.begin(), aSpans.end(), aMs, [](int32_t ms, const std::pair<int32_t, int32_t>& s) { return ms < s.first; });
		return it != aSpans.begin() && (it - 1)->second >= aMs;
	}

	int32_t TimeIn(const std::vector<std::pair<int32_t, int32_t>>& aSpans, int32_t aFrom, int32_t aTo)
	{
		int32_t n = 0;
		for (const auto& [a, b] : aSpans)
		{
			if (a >= aTo) { break; }
			n += std::max(0, std::min(b, aTo) - std::max(a, aFrom));
		}
		return n;
	}

	int SpikeAt(const Fight& f, bool aOurs, int32_t aMs)
	{
		const auto& peaks = aOurs ? f.OurSpikesMs : f.TheirSpikesMs;
		for (size_t i = 0; i < peaks.size(); i++)
		{
			auto [a, b] = RunOf(f, aOurs, i);
			if (aMs >= a - 1000 && aMs <= b + 1000) { return static_cast<int>(i); }
		}
		return -1;
	}

	int64_t StampSeconds(const Fight& f)
	{
		// "yyyymmdd-hhmmss"
		const std::string& st = f.Stamp;
		if (st.size() < 15 || st[8] != '-') { return 0; }
		auto num = [&](size_t aAt, size_t aLen)
		{
			int v = 0;
			for (size_t i = aAt; i < aAt + aLen; i++) { if (st[i] < '0' || st[i] > '9') { return -1; } v = v * 10 + (st[i] - '0'); }
			return v;
		};
		const int y = num(0, 4), mo = num(4, 2), d = num(6, 2), h = num(9, 2), mi = num(11, 2), s = num(13, 2);
		if (y < 0 || mo < 1 || d < 1 || h < 0 || mi < 0 || s < 0) { return 0; }
		// days since 2000-03-01 (civil calendar, March first so leap days fall at the end)
		const int yy = mo <= 2 ? y - 1 : y, mm = mo <= 2 ? mo + 9 : mo - 3;
		const int64_t days = 365LL * (yy - 2000) + (yy - 2000) / 4 - (yy - 2000) / 100 + (yy - 2000) / 400 + (153 * mm + 2) / 5 + d - 1;
		return days * 86400 + h * 3600 + mi * 60 + s;
	}

	int32_t ReviveRecharge(int32_t aSkill, int64_t aAtMs)
	{
		switch (aSkill)
		{
		case 9163: case 9243: return 90000;                       // Signet of Mercy
		case 5573: case 5760: case 5761: case 5762: case 5763: return 90000; // Glyph of Renewal and its attunements
		case 10244:                                              // Illusion of Life: 75 s in WvW until the 2025-02-11 balance update
		{
			static const int64_t change = [] { Fight g; g.Stamp = "20250211-000000"; return StampSeconds(g) * 1000; }();
			return aAtMs < change ? 75000 : 90000;
		}
		case 10611: return 75000;                                // Signet of Undeath
		case 14419: return 120000;                               // Battle Standard
		case 12569: return 120000;                               // Spirit of Nature
		default: return 0;
		}
	}

	bool IsReviveTool(const Player& p, int32_t aSkill)
	{
		if (!ReviveRecharge(aSkill)) { return false; }
		return aSkill != 14419 || p.Spec == "Paragon"; // only a Paragon's Battle Standard is a revive tool
	}

	Round Compute(const Fight& f, const Night& aNight, bool aRiskSets)
	{
		Round r;
		Ctx c(f);
		const int64_t roundStart = StampSeconds(f) * 1000 - f.DurationMs; // ms since 2000

		// Revive tools over the night: who carries which (they used it tonight on this spec), and every use's moment
		std::map<std::pair<std::string, std::string>, int32_t> carried; // (account, spec) -> tool
		std::vector<ToolUse> uses;
		for (size_t k = 0; k < aNight.Rounds.size(); k++)
		{
			const Fight& g = *aNight.Rounds[k];
			const int64_t start = StampSeconds(g) * 1000 - g.DurationMs;
			for (const Player& p : g.Players)
			{
				for (const auto& u : p.ReviveUses)
				{
					if (!IsReviveTool(p, u.Skill)) { continue; }
					carried[{p.Account, p.Spec}] = u.Skill;
					if (u.Done) { uses.push_back({p.Account, start + u.Ms, u.Skill}); }
				}
			}
		}
		const bool night = !aNight.Rounds.empty();

		// ---- ally downs, their risk sets and what became of them ----
		for (size_t i = 0; i < f.Players.size(); i++)
		{
			const Player& p = f.Players[i];
			for (size_t k = 0; k < p.DownSpans.size(); k++)
			{
				const auto& sp = p.DownSpans[k];
				if (sp.Dead) { continue; }
				AllyDown d;
				d.Ms = sp.From;
				d.Player = static_cast<int>(i);
				d.Died = k + 1 < p.DownSpans.size() && p.DownSpans[k + 1].Dead && std::abs(p.DownSpans[k + 1].From - sp.To) < 300;
				d.DownMs = sp.To - sp.From;
				for (const auto& h : p.HitsIn) { if (h.Ms >= sp.From && h.Ms <= sp.To) { d.Cleave += h.Damage; } }
				std::set<int> revivers;
				for (size_t j = 0; j < f.Players.size(); j++)
				{
					for (const auto& rv : f.Players[j].Reviving) { if (rv.Target == static_cast<int>(i) && rv.To >= sp.From && rv.From <= sp.To) { revivers.insert(static_cast<int>(j)); } }
				}
				d.Reviving = static_cast<int>(revivers.size());
				for (const auto& il : p.IllusionOfLife) { d.RevivedBySkill |= il.From >= sp.From - 200 && il.From <= sp.To + 200; }
				if (!d.Died && sp.To < f.DurationMs - 500)
				{
					for (size_t en = 0; en < f.Enemies.size() && !d.Rallied; en++)
					{
						for (const auto& es : f.Enemies[en].DownSpans)
						{
							if (!es.Dead || std::abs(es.From - sp.To) > 400) { continue; }
							for (const auto& h : p.HitsOut) { if (h.Enemy == static_cast<int>(en) && h.Ms <= es.From && h.Ms >= es.From - 15000) { d.Rallied = true; break; } }
							if (d.Rallied) { break; }
						}
					}
				}
				if (!d.Died)
				{
					// a revive skill that went off near them while they were down, and they were up within 3 s of it
					const auto* me = PosNear(p.Pos, sp.From, 2000);
					for (const Player& q : f.Players)
					{
						for (const auto& u : q.ReviveUses)
						{
							if (!u.Done || !ReviveRecharge(u.Skill) || u.Ms < sp.From || u.Ms > sp.To + 500 || sp.To - u.Ms > 3000) { continue; }
							const auto* at = PosNear(q.Pos, u.Ms, 2000);
							if (!me || !at || Dist(me->X, me->Y, at->X, at->Y) <= 1500) { d.RevivedBySkill = true; }
						}
					}
				}
				d.Spike = SpikeAt(f, false, sp.From);
				d.InEnemySpike = d.Spike >= 0;
				if (const auto* me = PosNear(p.Pos, sp.From, 2000))
				{
					for (size_t j = 0; j < f.Players.size(); j++)
					{
						if (j == i || !UpAt(f.Players[j], sp.From)) { continue; }
						if (const auto* q = PosNear(f.Players[j].Pos, sp.From); q && Dist(me->X, me->Y, q->X, q->Y) <= kNear) { d.AlliesNear++; }
					}
					for (const auto& e : f.Enemies)
					{
						if (!EnemyUp(e, sp.From)) { continue; }
						if (const auto* q = PosNear(e.Pos, sp.From); q && Dist(me->X, me->Y, q->X, q->Y) <= kNear) { d.EnemiesNear++; }
					}
					if (night)
					{
						// carriers up within reach whose tool had recharged (uses before this moment, tonight)
						d.ToolsReady = 0;
						d.ToolsReadyStrict = 0;
						const int64_t now = roundStart + sp.From;
						for (size_t j = 0; j < f.Players.size(); j++)
						{
							const Player& q = f.Players[j];
							auto tool = carried.find({q.Account, q.Spec});
							if (j == i || tool == carried.end() || !UpAt(q, sp.From)) { continue; }
							const auto* at = PosNear(q.Pos, sp.From);
							if (!at || Dist(me->X, me->Y, at->X, at->Y) > 1500) { continue; }
							bool ready = true, strict = Dist(me->X, me->Y, at->X, at->Y) <= 1200;
							for (const ToolUse& u : uses)
							{
								if (u.Account == q.Account && u.At <= now && now - u.At < ReviveRecharge(u.Skill, u.At) * 85 / 100) { ready = false; } // alacrity: about 15% sooner
								if (u.Account == q.Account && u.At <= now && now - u.At < ReviveRecharge(u.Skill, u.At)) { strict = false; }
							}
							d.ToolsReadyStrict += ready && strict;
							d.ToolsReady += ready;
							if (ready) { d.Ready.push_back(static_cast<int>(j)); }
						}
					}
				}
				for (const Player& q : f.Players)
				{
					for (const auto& u : q.ReviveUses) { d.ToolsSpentBefore += u.Done && IsReviveTool(q, u.Skill) && u.Ms < sp.From && u.DownNear == 0; }
				}
				// the risk set: allies up just before and under fire in the window (the downed one included)
				for (size_t j = 0; j < f.Players.size(); j++)
				{
					const Player& q = f.Players[j];
					if (j != i)
					{
						if (!aRiskSets || !UpAt(q, sp.From - 50)) { continue; }
						bool fire = false;
						for (size_t h = From(q.HitsIn, sp.From - kLookBack, [](const Player::TakenHit& x) { return x.Ms; }); h < q.HitsIn.size() && q.HitsIn[h].Ms <= sp.From; h++)
						{
							if (q.HitsIn[h].Damage > 0) { fire = true; break; }
						}
						if (!fire) { continue; }
					}
					AllyState s = AllyAt(c, static_cast<int>(j), sp.From);
					s.Case = j == i;
					d.Risk.push_back(s);
				}
				r.AllyDowns.push_back(std::move(d));
			}
		}
		std::sort(r.AllyDowns.begin(), r.AllyDowns.end(), [](const AllyDown& a, const AllyDown& b) { return a.Ms < b.Ms; });

		// ---- enemy downs ----
		for (size_t i = 0; i < f.Enemies.size(); i++)
		{
			const auto& e = f.Enemies[i];
			for (size_t k = 0; k < e.DownSpans.size(); k++)
			{
				const auto& sp = e.DownSpans[k];
				if (sp.Dead) { continue; }
				EnemyDown d;
				d.Ms = sp.From;
				d.Enemy = static_cast<int>(i);
				d.Died = k + 1 < e.DownSpans.size() && e.DownSpans[k + 1].Dead && std::abs(e.DownSpans[k + 1].From - sp.To) < 300;
				d.DownMs = sp.To - sp.From;
				d.EndOfLog = !d.Died && sp.To >= f.DurationMs - 500;
				std::set<int> hitters, early;
				for (const auto& h : c.OnEnemy[i])
				{
					if (h.Ms < sp.From || h.Ms > sp.To) { continue; }
					d.Cleave += h.Damage;
					hitters.insert(h.Player);
					if (d.FirstHit < 0) { d.FirstHit = h.Ms - sp.From; }
					if (h.Ms - sp.From <= 1500) { d.CleaveEarly += h.Damage; early.insert(h.Player); }
				}
				d.Hitters = static_cast<int>(hitters.size());
				d.HittersEarly = static_cast<int>(early.size());
				if (!d.Died && !d.EndOfLog)
				{
					// rallied: an ally died within 0.4 s of them getting up, and they had hit that ally in the 15 s before
					for (const Player& q : f.Players)
					{
						for (const auto& qs : q.DownSpans)
						{
							if (!qs.Dead || std::abs(qs.From - sp.To) > 400) { continue; }
							for (const auto& h : q.HitsIn) { if (h.Enemy == static_cast<int>(i) && h.Ms <= qs.From && h.Ms >= qs.From - 15000) { d.Rallied = true; break; } }
						}
						if (d.Rallied) { break; }
					}
				}
				d.Spike = SpikeAt(f, true, sp.From);
				for (size_t j = 0; j < f.Enemies.size(); j++)
				{
					if (j != i)
					{
						if (!aRiskSets || !EnemyUp(f.Enemies[j], sp.From - 50)) { continue; }
						const auto& on = c.OnEnemy[j];
						size_t h = From(on, sp.From - kLookBack, [](const Ctx::HitRef& x) { return x.Ms; });
						if (h >= on.size() || on[h].Ms > sp.From) { continue; }
					}
					EnemyState s = EnemyAt(c, static_cast<int>(j), sp.From);
					s.Case = j == i;
					d.Risk.push_back(s);
				}
				r.EnemyDowns.push_back(std::move(d));
			}
		}
		std::sort(r.EnemyDowns.begin(), r.EnemyDowns.end(), [](const EnemyDown& a, const EnemyDown& b) { return a.Ms < b.Ms; });

		// ---- pushes ----
		for (int side = 0; side < 2; side++)
		{
			const bool ally = side == 0;
			const auto& peaks = ally ? f.OurSpikesMs : f.TheirSpikesMs;
			for (size_t k = 0; k < peaks.size(); k++)
			{
				Push u;
				u.Ally = ally;
				u.Index = static_cast<int>(k);
				u.Peak = static_cast<int32_t>(peaks[k]);
				std::tie(u.First, u.Last) = RunOf(f, ally, k);
				const int32_t a = u.First - 1000, b = u.Last + 3000;
				if (ally)
				{
					for (const EnemyDown& d : r.EnemyDowns) { if (d.Ms >= a && d.Ms <= b) { u.Downs++; u.Deaths += d.Died; } }
					std::map<int, int64_t> byTarget;
					for (size_t i = 0; i < f.Players.size(); i++)
					{
						const Player& p = f.Players[i];
						if (!UpAt(p, u.Peak)) { continue; }
						u.Up++;
						u.DamageUp += c.DamagePlayer[i];
						bool hit = false;
						for (size_t h = From(p.HitsOut, u.First - 1500, [](const Player::TakenHit& x) { return x.Ms; }); h < p.HitsOut.size() && p.HitsOut[h].Ms <= u.Last + 1500; h++)
						{
							const auto& x = p.HitsOut[h];
							if (x.Damage <= 0) { continue; }
							hit = true;
							if (x.Ms < u.First - 1000 || x.Ms > u.Last + 1000 || x.Enemy < 0) { continue; }
							byTarget[x.Enemy] += x.Damage; u.Damage += x.Damage;
							if (x.Ms >= u.Peak - 1500 && x.Ms <= u.Peak + 1500) { u.Sync += x.Damage; }
						}
						u.Hitting += hit;
						u.DamageHitting += hit && c.DamagePlayer[i];
					}
					u.Targets = static_cast<int>(byTarget.size());
					int64_t top = 0;
					for (auto& [en, v] : byTarget) { top = std::max(top, v); }
					u.TopTargetShare = u.Damage > 0 ? double(top) / u.Damage : 0;
					u.Sync = u.Damage > 0 ? u.Sync / u.Damage : 0;
					// focus: damage on an enemy 10+ allies hit within 1.5 s of the hit, of the damage on enemies up
					int64_t focused = 0, live = 0;
					for (const auto& on : c.OnEnemy)
					{
						// live targets only: hits on a downed enemy are the cleave, not focus
						const Fight::Enemy& target = f.Enemies[static_cast<size_t>(&on - c.OnEnemy.data())];
						std::vector<std::pair<int32_t, int>> hits;
						for (const auto& x : on) { hits.push_back({x.Ms, EnemyUp(target, x.Ms) ? x.Player : -1}); }
						const std::vector<bool> many = Focused(hits, u.First - 1000, u.Last + 1000, f.Players.size());
						for (size_t h = 0; h < on.size(); h++)
						{
							if (on[h].Ms < u.First - 1000 || on[h].Ms > u.Last + 1000 || hits[h].second < 0) { continue; }
							live += on[h].Damage;
							if (many[h]) { focused += on[h].Damage; }
						}
					}
					u.Focus = live > 0 ? double(focused) / live : 0;
					for (const auto& e : f.Enemies)
					{
						for (const auto& t : e.CcIn) { u.CcBefore += t.Ms >= u.First - 3000 && t.Ms < u.First; u.CcIn += t.Ms >= u.First && t.Ms <= u.Last + 1000; }
						for (const auto& t : e.StabBlocked) { u.Blocked += t.Ms >= u.First - 3000 && t.Ms <= u.Last + 1000; }
						for (const auto& t : e.StripsIn)
						{
							if (t.Ms < u.First - 3000 || t.Ms > u.First) { continue; }
							u.StripsBefore++;
							u.StabStripsBefore += t.Boon == kStability;
						}
						u.Immobilized += EnemyUp(e, u.Peak) && OnAt(e.ImmobOn, u.Peak);
					}
					for (int32_t w : c.Wells) { u.Wells += w >= u.First - 3000 && w <= u.First + 1500; }
					for (int32_t sec = std::max(0, u.First / 1000); sec <= u.Last / 1000 && sec < static_cast<int32_t>(f.InvulnTheirs.size()); sec++) { u.Negated += static_cast<int>(f.InvulnTheirs[sec].size()); }
				}
				else
				{
					for (const AllyDown& d : r.AllyDowns) { if (d.Ms >= a && d.Ms <= b) { u.Downs++; u.Deaths += d.Died; } }
					std::set<int> hitters;
					std::map<int, int64_t> byTarget;
					for (size_t i = 0; i < f.Players.size(); i++)
					{
						const Player& p = f.Players[i];
						bool hit = false;
						for (size_t h = From(p.HitsIn, u.First - 1500, [](const Player::TakenHit& x) { return x.Ms; }); h < p.HitsIn.size() && p.HitsIn[h].Ms <= u.Last + 1500; h++)
						{
							const auto& x = p.HitsIn[h];
							if (x.Damage <= 0) { continue; }
							hit = true;
							if (x.Enemy >= 0) { hitters.insert(x.Enemy); }
							if (x.Ms < u.First - 1000 || x.Ms > u.Last + 1000) { continue; }
							byTarget[static_cast<int>(i)] += x.Damage; u.Damage += x.Damage;
							if (x.Ms >= u.Peak - 1500 && x.Ms <= u.Peak + 1500) { u.Sync += x.Damage; }
						}
						for (const auto& h : p.CcIn) { u.CcBefore += h.Ms >= u.First - 3000 && h.Ms < u.First; u.CcIn += h.Ms >= u.First && h.Ms <= u.Last + 1000; }
						for (const auto& st : p.StripsIn)
						{
							if (st.Ms < u.First - 3000 || st.Ms > u.First) { continue; }
							u.StripsBefore++;
							u.StabStripsBefore += st.Boon == kStability;
						}
						if (hit && UpAt(p, u.First - 1500))
						{
							u.HitAllies++;
							u.HitWithStab += OnAt(p.BoonOn[kStability], u.Peak);
							bool stripped = false, ccd = false;
							for (const auto& [ms, kind] : p.StabLost) { stripped |= kind == 0 && ms >= u.First - 3000 && ms <= u.Last; }
							for (const auto& h : p.CcIn) { ccd |= h.Ms >= u.First - 3000 && h.Ms <= u.Last + 500; }
							u.HitStripped += stripped;
							u.HitCcd += ccd;
						}
					}
					for (size_t j = 0; j < f.Enemies.size(); j++) { u.Up += f.Enemies[j].Fought && EnemyUp(f.Enemies[j], u.Peak); }
					u.Hitting = static_cast<int>(hitters.size());
					u.Targets = static_cast<int>(byTarget.size());
					int64_t top = 0;
					for (auto& [pl, v] : byTarget) { top = std::max(top, v); }
					u.TopTargetShare = u.Damage > 0 ? double(top) / u.Damage : 0;
					u.Sync = u.Damage > 0 ? u.Sync / u.Damage : 0;
					// focus: damage on an ally 10+ enemies hit within 1.5 s of the hit, of the damage on allies up
					int64_t focused = 0, live = 0;
					for (const Player& p : f.Players)
					{
						std::vector<std::pair<int32_t, int>> hits;
						for (const auto& x : p.HitsIn) { hits.push_back({x.Ms, x.Damage > 0 && UpAt(p, x.Ms) ? x.Enemy : -1}); }
						const std::vector<bool> many = Focused(hits, u.First - 1000, u.Last + 1000, f.Enemies.size());
						for (size_t h = 0; h < p.HitsIn.size(); h++)
						{
							const auto& x = p.HitsIn[h];
							if (x.Ms < u.First - 1000 || x.Ms > u.Last + 1000 || hits[h].second < 0) { continue; }
							live += x.Damage;
							if (many[h]) { focused += x.Damage; }
						}
					}
					u.Focus = live > 0 ? double(focused) / live : 0;
					for (const auto& n : f.NegatedHits) { u.Negated += n.Ms >= u.First - 1000 && n.Ms <= u.Last + 1000 && n.Kind != 4; }
				}
				const Pt sq = c.SquadMid(u.First), en = c.EnemyMid(u.First);
				if (sq.Ok && en.Ok) { u.Gap = Dist(sq.X, sq.Y, en.X, en.Y); }
				r.Pushes.push_back(u);
			}
		}
		r.AllyStopMs = StopMs(f.ToPlayersPerS);
		r.EnemyStopMs = StopMs(f.InPerS);
		for (AllyDown& d : r.AllyDowns) { d.AfterStop = r.AllyStopMs >= 0 && d.Ms >= r.AllyStopMs; }
		for (EnemyDown& d : r.EnemyDowns) { d.AfterStop = r.EnemyStopMs >= 0 && d.Ms >= r.EnemyStopMs; }
		return r;
	}

	namespace
	{
#include "CauseRefs.inc"
	}

	int32_t StopMs(const std::vector<int64_t>& aPerS) { return StopAt(aPerS); }

	const SizeBand& BandOf(int aAllies)
	{
		for (const SizeBand& b : kBands) { if (aAllies <= b.MaxAllies) { return b; } }
		return kBands[std::size(kBands) - 1];
	}

	bool HasFactor(const AllyState& s, int aFactor, const Fight& f)
	{
		switch (aFactor)
		{
		case F_Cc: return s.Cc >= 1;
		case F_Immob: return s.ImmobMs > 0;
		case F_NoProt: return !s.Protection;
		case F_CcKite: case F_RanPast:
		{
			// 300+ further toward the enemy than the squad's middle went in the 3 s, with positions known; not the tag (leading is
			// their job). The squad's middle went 200+ back from the enemy (a kite) and they were CC'd, rooted or slowed then: CC'd
			// during the kite. Otherwise, and not pulled or taunted toward the enemy: ran past the squad.
			if (s.Player == f.Commander || s.MovedIn < -99000 || s.SquadMovedIn < -99000 || s.MovedIn - s.SquadMovedIn < 300) { return false; }
			const bool kite = s.SquadMovedIn < -200;
			if (aFactor == F_CcKite) { return kite && (s.Cc >= 1 || s.HardMs > 0); }
			return !kite && s.Pulled == 0;
		}
		case F_Hurt: return s.HpBefore >= 0 && s.HpBefore < 9000;
		default: return false;
		}
	}

	Summary Summarize(const Fight& f, const Round& r)
	{
		Summary s;
		s.Downs = static_cast<int>(r.AllyDowns.size());
		std::vector<int32_t> ccToDown;
		std::map<int, int> unused;
		const auto& odds = BandOf(f.SquadCount).Odds; // the odds for fights of this size
		s.AllyStopMs = r.AllyStopMs;
		for (const AllyDown& d : r.AllyDowns)
		{
			if (d.AfterStop)
			{
				// after allies stopped fighting: counted apart, in no line
				s.Flags.push_back(0);
				s.AfterStopDowns++;
				s.Deaths += d.Died;
				s.AfterStopDeaths += d.Died;
				continue;
			}
			const AllyState* c = nullptr;
			for (const AllyState& m : d.Risk) { if (m.Case) { c = &m; } }
			uint8_t flags = 0;
			double total = 0;
			for (int k = 0; k < F_Count; k++)
			{
				if (c && HasFactor(*c, k, f)) { flags |= static_cast<uint8_t>(1u << k); s.With[k]++; total += std::log(odds[k]); }
			}
			s.Flags.push_back(flags);
			if (total > 0)
			{
				const double af = 1 - std::exp(-total);
				for (int k = 0; k < F_Count; k++) { if (flags & (1u << k)) { s.Cost[k] += af * std::log(odds[k]) / total; } }
			}
			if (c && (flags & (1u << F_Cc)))
			{
				s.CcStabStripped += c->StabStripped;
				if (c->SinceCc >= 0) { ccToDown.push_back(c->SinceCc); }
			}
			if (!d.Died) { continue; }
			s.Deaths++;
			if (d.DownMs < 1500) { s.DiedFast++; }
			else if (d.ToolsReady >= 2) { s.DiedUnused++; for (int q : d.Ready) { unused[q]++; } }
			else if (d.ToolsReady >= 0) { s.DiedRanOut++; }
		}
		if (!ccToDown.empty()) { std::sort(ccToDown.begin(), ccToDown.end()); s.CcToDownMs = ccToDown[ccToDown.size() / 2]; }
		std::vector<std::pair<int, int>> by;
		for (auto& [q, n] : unused) { by.push_back({n, q}); }
		std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
		for (auto& [n, q] : by) { s.UnusedBy.push_back(q); }
		for (size_t i = 0; i < f.Players.size(); i++)
		{
			const Player& p = f.Players[i];
			for (const auto& u : p.ReviveUses)
			{
				if (!u.Done || !IsReviveTool(p, u.Skill)) { continue; }
				s.ToolUses++;
				s.LastToolMs = std::max(s.LastToolMs, u.Ms);
				if (u.DownNear == 0) { s.ToolsOnNobody++; s.OnNobody.push_back({static_cast<int>(i), u.Ms}); }
			}
		}
		std::sort(s.OnNobody.begin(), s.OnNobody.end(), [](auto& a, auto& b) { return a.second < b.second; });

		// the enemy side
		s.EnemyDowns = static_cast<int>(r.EnemyDowns.size());
		std::vector<int64_t> cleave;
		for (const EnemyDown& d : r.EnemyDowns) { s.EnemyDeaths += d.Died; cleave.push_back(d.Cleave); }
		if (!cleave.empty()) { std::sort(cleave.begin(), cleave.end()); s.CleaveMedian = cleave[cleave.size() / 2]; }
		{
			std::vector<int> hitters;
			std::vector<int64_t> early;
			for (const EnemyDown& d : r.EnemyDowns)
			{
				if (d.AfterStop) { continue; } // the enemy had stopped fighting: picking off the rest
				s.EnemyRallied += d.Rallied;
				if (d.EndOfLog) { continue; }
				hitters.push_back(d.HittersEarly);
				early.push_back(d.CleaveEarly);
			}
			if (hitters.size() >= 2)
			{
				std::sort(hitters.begin(), hitters.end());
				std::sort(early.begin(), early.end());
				const size_t n = hitters.size();
				s.HittersEarly = n % 2 ? hitters[n / 2] : (hitters[n / 2 - 1] + hitters[n / 2]) / 2.0;
				s.CleaveEarly = early[n / 2];
			}
		}
		double focus = 0, damage = 0;
		int cc = 0, blocked = 0, before = 0;
		for (const Push& u : r.Pushes)
		{
			if (!u.Ally) { continue; }
			s.AllyPushes++;
			s.PushesWithDowns += u.Downs > 0;
			focus += u.Focus * static_cast<double>(u.Damage);
			damage += static_cast<double>(u.Damage);
			cc += u.CcBefore + u.CcIn;
			blocked += u.Blocked;
			before += u.CcBefore;
		}
		if (damage > 0) { s.Focus = focus / damage; }
		if (cc + blocked > 0) { s.CcLanded = double(cc) / (cc + blocked); }
		if (s.AllyPushes) { s.CcBefore = double(before) / s.AllyPushes; }

		// waves: each enemy push's downs
		std::map<int, size_t> wave; // push -> into Waves
		for (size_t i = 0; i < r.AllyDowns.size(); i++)
		{
			const AllyDown& d = r.AllyDowns[i];
			if (d.Spike < 0 || d.AfterStop) { continue; }
			auto it = wave.find(d.Spike);
			if (it == wave.end())
			{
				auto [a, b] = RunOf(f, false, static_cast<size_t>(d.Spike));
				Wave w;
				w.Push = d.Spike; w.From = a; w.To = b;
				it = wave.emplace(d.Spike, s.Waves.size()).first;
				s.Waves.push_back(w);
			}
			Wave& w = s.Waves[it->second];
			w.Downs++;
			w.Deaths += d.Died;
			w.Cc += (s.Flags[i] >> F_Cc) & 1;
			for (const AllyState& m : d.Risk) { if (m.Case) { w.StabStripped += m.StabStripped; } }
			if (d.ToolsReady >= 0 && (w.ToolsReady < 0 || d.ToolsReady < w.ToolsReady)) { w.ToolsReady = d.ToolsReady; }
			w.Downed.push_back(static_cast<int>(i));
		}
		for (Wave& w : s.Waves)
		{
			std::map<int, int> sg;
			for (int i : w.Downed) { sg[f.Players[r.AllyDowns[i].Player].Subgroup]++; }
			for (auto& [g, n] : sg) { if (n > w.SubgroupDowns) { w.SubgroupDowns = n; w.Subgroup = g; } }
		}
		std::stable_sort(s.Waves.begin(), s.Waves.end(), [](const Wave& a, const Wave& b) { return a.Downs + a.Deaths > b.Downs + b.Deaths; });
		return s;
	}
}
