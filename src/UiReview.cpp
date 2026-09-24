// Review tab: the answer, what to fix (each line opens in place), your measures against your spec, and what held
// you back. A measure opens it by skill (Review > Healing /s), a skill the skill detail.
#include <algorithm>
#include <cmath>
#include <set>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "SkillIcons.h"
#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		constexpr int kNoMetric = -1;
		enum FixKind { F_Skill, F_Measure, F_Cc, F_Down };

		struct Fix
		{
			double      Score = 0;
			std::string Key;              // stable across rounds, for "N of M rounds"
			FixKind     Kind = F_Skill;
			int32_t     Skill = kNoSkill;
			int         Metric = kNoMetric;
			std::string Subject, What, Unit, YouText, ThemText;
			double      You = 0, Them = 0;
		};

		bool InEnemySpike(const Fight& f, int32_t aMs)
		{
			for (int64_t t : f.TheirSpikesMs) { if (aMs >= t - 1000 && aMs <= t + 4000) { return true; } }
			return false;
		}

		// The few places where you fell behind the compared player, biggest first
		std::vector<Fix> FindFixes(const Ctx& c)
		{
			std::vector<Fix> out;
			if (!c.MeRaw || !c.Vs) { return out; }
			const Player& you = c.You;
			const Player& vs = *c.Vs;
			const Metric& m = Metrics()[c.RoleMetric];

			// Skills of your role's main output
			if (Known(m, you) && Known(m, vs))
			{
				double total = std::max(Rate(m, vs, m.Total(vs)), 1e-9);
				std::set<int32_t> skills;
				for (auto& [s, r] : you.Skills) { skills.insert(s); }
				for (auto& [s, r] : vs.Skills) { skills.insert(s); }
				for (int32_t s : skills)
				{
					if (s == 0) { continue; } // "other sources" isn't something to press
					Why w = Explain(you, vs, vs.Name, c.RoleMetric, s, c.OneRound);
					if (w.Gap <= 0.03 * total) { continue; }
					Fix f;
					f.Score = w.Gap / total * (w.Word == "no casts" ? 0.6 : 1.0);
					f.Key = "s:" + std::to_string(s);
					f.Skill = s; f.Metric = c.RoleMetric; f.Subject = c.Name(s); f.What = w.Word;
					// a boon's own ticks (Regeneration's healing) read like the boon measure: say what it is
					for (const char* boon : Analysis::kBoonNames) { if (f.Subject == boon) { f.Subject = m.Name + " from " + f.Subject; } }
					f.You = w.You; f.Them = w.Them; f.YouText = w.YouText; f.ThemText = w.ThemText; f.Unit = w.Unit;
					out.push_back(f);
				}
			}

			// Other measures where the compared player clearly did more
			auto measure = [&](int aMetric, const std::string& aSubject, double y, double t, const std::string& aUnit, bool aEnough, double aWeight)
			{
				if (!aEnough || t <= 0) { return; }
				Fix f;
				f.Kind = F_Measure; f.Metric = aMetric; f.Key = "m:" + aSubject; f.Subject = aSubject;
				f.What = y <= 0 ? "none" : "lower";
				f.You = y; f.Them = t; f.YouText = Num(y); f.ThemText = Num(t); f.Unit = aUnit;
				f.Score = (t - y) / t * aWeight;
				out.push_back(f);
			};
			for (int metric : {kMetricCleanses, kMetricStrips})
			{
				if (metric == c.RoleMetric) { continue; }
				const Metric& mm = Metrics()[metric];
				double y = Rate(mm, you, mm.Total(you)), t = Rate(mm, vs, mm.Total(vs));
				measure(metric, mm.Name, y, t, "per minute", t - y >= 2.0 && t >= 1.25 * y, 0.6);
			}
			for (int b = 0; b < Analysis::kBoons; b++)
			{
				if (kMetricGroupBoon + b == c.RoleMetric) { continue; }
				double y = GroupGenOver(c.Scope, you.Account, you.Spec, b), t = GroupGenOver(c.Scope, vs.Account, vs.Spec, b);
				bool stacks = c.F->Intensity[b];
				measure(kMetricGroupBoon + b, std::string(Analysis::kBoonNames[b]) + " on your subgroup", y, t, stacks ? "stacks" : "% uptime",
					t - y >= (stacks ? 0.3 : 8.0) && t >= 1.3 * y, 0.5);
			}
			if (you.StabEligible >= 3 && vs.StabEligible >= 3)
			{
				double y = 100.0 * you.StabCovered / you.StabEligible, t = 100.0 * vs.StabCovered / vs.StabEligible;
				measure(kMetricGroupBoon + Analysis::kStability, "CC covered by your stability", y, t, "% of CC on your subgroup", t - y >= 15, 0.9);
			}

			// What held you back
			if (you.CcTaken >= vs.CcTaken + 2 && you.CcTaken >= 1.5 * vs.CcTaken)
			{
				Fix f;
				f.Kind = F_Cc; f.Key = "cc"; f.Subject = "Crowd controlled";
				f.What = vs.CcTaken ? std::to_string(int(std::lround(double(you.CcTaken) / vs.CcTaken))) + "x as often" : "more often";
				f.You = you.CcTaken; f.Them = vs.CcTaken; f.YouText = std::to_string(you.CcTaken); f.ThemText = std::to_string(vs.CcTaken);
				f.Unit = "times, " + std::to_string(you.CcNoStab) + " with no stability just before";
				f.Score = 0.25 + 0.02 * (you.CcTaken - vs.CcTaken);
				out.push_back(f);
			}
			double downY = (you.DownedMs + DeadMs(you)) / 1000.0, downT = (vs.DownedMs + DeadMs(vs)) / 1000.0;
			if (downY >= downT + 5)
			{
				Fix f;
				f.Kind = F_Down; f.Key = "down"; f.Subject = you.Deaths ? "Died" : "Downed";
				f.What = you.Deaths + you.Downs > 1 ? std::to_string(std::max(you.Deaths, you.Downs)) + " times" : "once";
				if (c.OneRound && c.MeRaw && !c.MeRaw->DownSpans.empty() && InEnemySpike(*c.F, c.MeRaw->DownSpans[0].From)) { f.What = "in an enemy spike"; }
				f.You = downY; f.Them = downT; f.YouText = Num(downY); f.ThemText = Num(downT);
				f.Unit = "s downed or dead" + (c.OneRound && c.MeRaw && !c.MeRaw->DownSpans.empty() ? ", first at " + Duration(c.MeRaw->DownSpans[0].From) : std::string());
				f.Score = 0.3;
				out.push_back(f);
			}
			std::sort(out.begin(), out.end(), [](const Fix& a, const Fix& b) { return a.Score > b.Score; });
			if (out.size() > 5) { out.resize(5); }
			return out;
		}

		// "N of M rounds": the same fix in earlier rounds, each against that round's compared player
		std::string Recurring(const Ctx& c, const std::string& aKey)
		{
			static std::string cacheKey;
			static std::map<int, std::set<std::string>> perRound;
			static int rounds = 0;
			const auto& fights = *c.Fights;
			std::string key = std::to_string(fights.size()) + "|" + c.You.Account + "|" + c.You.Spec + "|" + S().VsAccount;
			if (key != cacheKey)
			{
				cacheKey = key;
				perRound.clear();
				rounds = 0;
				for (int i = 0; i < static_cast<int>(fights.size()); i++)
				{
					const Fight& f = *fights[i];
					if (f.Pov < 0 || f.Players[f.Pov].Account != c.You.Account || f.Players[f.Pov].Spec != c.You.Spec) { continue; }
					rounds++;
					Ctx ci = BuildCtx(fights, i, false);
					for (const Fix& x : FindFixes(ci)) { perRound[i].insert(x.Key); }
				}
			}
			int n = 0;
			for (auto& [i, keys] : perRound) { n += keys.count(aKey) ? 1 : 0; }
			return n >= 2 ? "in " + std::to_string(n) + " of " + std::to_string(rounds) + " rounds tonight" : std::string();
		}

		// Two thin bars, you and the compared player; the numbers follow at full size (see FixLine)
		void PairBars(ImDrawList* dl, ImVec2 p, float aWidth, float aHeight, const Fix& f)
		{
			double mx = std::max(f.You, f.Them);
			if (mx <= 0) { mx = 1; }
			float bh = std::max(3.0f, aHeight * 0.30f);
			float y1 = p.y + aHeight * 0.5f - bh - 1, y2 = p.y + aHeight * 0.5f + 1;
			float w1 = static_cast<float>(aWidth * f.You / mx), w2 = static_cast<float>(aWidth * f.Them / mx);
			Rect(dl, ImVec2(p.x, y1), w1, bh, kYou);
			Rect(dl, ImVec2(p.x, y2), w2, bh, kPeer);
		}

		// One fix line; returns true when clicked
		bool FixLine(int aIndex, const Fix& f, bool aOpen, const std::string& aTag)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			float lh = ImGui::GetTextLineHeight();
			float h = lh * 1.7f;
			ImVec2 p = ImGui::GetCursorScreenPos();
			float width = ImGui::GetContentRegionAvail().x;
			ImGui::PushID(aIndex);
			bool clicked = ImGui::Selectable("##fix", aOpen, 0, ImVec2(0, h));
			ImGui::PopID();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			float ty = p.y + (h - lh) * 0.5f;
			ImGui::RenderArrow(dl, ImVec2(p.x + 2, ty), muted, aOpen ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
			dl->AddText(ImVec2(p.x + 18, ty), muted, std::to_string(aIndex + 1).c_str());
			float x = p.x + 32;
			if (f.Kind == F_Skill)
			{
				if (void* icon = SkillIcons::Get(f.Skill, f.Subject)) { dl->AddImage(icon, ImVec2(x, ty), ImVec2(x + lh, ty + lh)); }
				x += lh + 5;
			}
			dl->AddText(ImVec2(x, ty), ink, f.Subject.c_str());
			dl->AddText(ImVec2(p.x + 250, ty), ink, f.What.c_str());
			PairBars(dl, ImVec2(p.x + 380, p.y), 130, h, f);
			std::string values = f.YouText + " / " + f.ThemText;
			dl->AddText(ImVec2(p.x + 520, ty), ink, values.c_str());
			dl->AddText(ImVec2(p.x + 528 + ImGui::CalcTextSize(values.c_str()).x, ty), muted, f.Unit.c_str());
			if (!aTag.empty())
			{
				if (ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x > p.x + width - ImGui::CalcTextSize(aTag.c_str()).x - 8)
				{
					ImGui::SetTooltip("The same came up in your other rounds tonight");
				}
				float tw = ImGui::CalcTextSize(aTag.c_str()).x;
				dl->AddText(ImVec2(p.x + width - tw - 4, ty), muted, aTag.c_str());
			}
			return clicked;
		}

		// ---- measures ------------------------------------------------------------------------------------------------

		struct MeasureRow
		{
			std::string Name, YouText, RefText, Gap;
			double You = 0, Best = 0, Median = 0;
			int Metric = kNoMetric;     // opens by skill
			bool Unknown = false;
			double Order = 0;
		};

		double Median(std::vector<double> v)
		{
			if (v.empty()) { return 0; }
			std::sort(v.begin(), v.end());
			return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
		}

		// Your output measures against your spec, biggest gap first; only what your role or your peers produced
		std::vector<MeasureRow> OutputRows(const Ctx& c)
		{
			struct Def { std::string Name; int Metric; std::function<double(const Player&)> Value; std::function<std::string(double)> Fmt; double Floor; bool NeedsHeal; };
			auto rate = [](int aMetric) { return [aMetric](const Player& p) { const Metric& m = Metrics()[aMetric]; return Rate(m, p, m.Total(p)); }; };
			std::vector<Def> defs = {
				{"Healing /s", kMetricHeal, rate(kMetricHeal), Num, 50, true},
				{"Barrier /s", kMetricBarrier, rate(kMetricBarrier), Num, 50, true},
				{"Healing on downed allies /s", kNoMetric, [](const Player& p) { return PerS(p, double(p.HealDowned)); }, Num, 50, true},
				{"Damage to players /s", kMetricDamage, rate(kMetricDamage), Num, 100, false},
				{"Cleanses /min", kMetricCleanses, rate(kMetricCleanses), Num, 1, false},
				{"Strips /min", kMetricStrips, rate(kMetricStrips), Num, 1, false},
			};
			if (c.MyRole == R_Damage) { defs.push_back({"Damage to all /s", kMetricDamageAll, rate(kMetricDamageAll), Num, 100, false}); }
			if (c.You.StabEligible >= 3)
			{
				defs.push_back({"CC covered by your stability", kMetricGroupBoon + Analysis::kStability,
					[](const Player& p) { return p.StabEligible >= 3 ? 100.0 * p.StabCovered / p.StabEligible : 0.0; },
					[](double v) { return std::to_string(int(v + 0.5)) + "%"; }, 1, false});
			}
			for (int b = 0; b < Analysis::kBoons; b++)
			{
				bool stacks = c.F->Intensity[b];
				const std::vector<FightPtr>& scope = c.Scope;
				defs.push_back({std::string(Analysis::kBoonNames[b]) + " on subgroup", kMetricGroupBoon + b,
					[&scope, b](const Player& p) { return GroupGenOver(scope, p.Account, p.Spec, b); },
					[stacks](double v) { return stacks ? Num(v) : std::to_string(int(v + 0.5)) + "%"; }, stacks ? 0.3 : 5.0, false});
			}
			std::vector<MeasureRow> rows;
			for (const Def& d : defs)
			{
				MeasureRow r;
				r.Name = d.Name; r.Metric = d.Metric;
				r.Unknown = d.NeedsHeal && !c.You.HealKnown;
				r.You = r.Unknown ? 0 : d.Value(c.You);
				std::vector<double> all{r.You};
				for (const Player& p : c.Peers)
				{
					if (d.NeedsHeal && !p.HealKnown) { continue; }
					double v = d.Value(p);
					r.Best = std::max(r.Best, v);
					all.push_back(v);
				}
				r.Median = Median(all);
				if (std::max(r.You, r.Best) < d.Floor) { continue; }
				r.YouText = r.Unknown ? "unknown" : d.Fmt(r.You);
				r.RefText = d.Fmt(r.Best);
				if (r.Unknown) { r.Gap = "no Healing Stats data"; r.Order = -1; }
				else if (r.You >= r.Best) { r.Gap = "the best"; r.Order = -0.5; }
				else
				{
					double rel = (r.Best - r.You) / r.Best;
					r.Order = rel;
					r.Gap = rel < 0.05 ? "about the best" : std::to_string(int(rel * 100 + 0.5)) + "% below best";
				}
				rows.push_back(r);
			}
			// Your role's main measure first, then the rest by gap
			int lead = c.MyRole == R_Heal ? kMetricHeal : c.MyRole == R_Stab ? kMetricGroupBoon + Analysis::kStability : kMetricDamage;
			const char* leadName = c.MyRole == R_Stab ? "CC covered by your stability" : nullptr;
			auto isLead = [&](const MeasureRow& r) { return leadName ? r.Name == leadName : r.Metric == lead; };
			std::stable_sort(rows.begin(), rows.end(), [&](const MeasureRow& a, const MeasureRow& b)
			{
				if (isLead(a) != isLead(b)) { return isLead(a); }
				return a.Order > b.Order;
			});
			if (rows.size() > 8) { rows.resize(8); }
			return rows;
		}

		// What held you back: fewer is better, against your spec's median
		std::vector<MeasureRow> HeldBackRows(const Ctx& c)
		{
			struct Def { std::string Name; std::function<double(const Player&)> Value; std::function<std::string(double)> Fmt; };
			auto secs = [](double v) { return Num(v) + " s"; };
			std::vector<Def> defs = {
				{"Crowd controlled", [](const Player& p) { return double(p.CcTaken); }, [](double v) { return Num(v) + "x"; }},
				{"Time downed", [](const Player& p) { return p.DownedMs / 1000.0; }, secs},
				{"Time dead", [](const Player& p) { return DeadMs(p) / 1000.0; }, secs},
				{"Casts cut short", [](const Player& p) { return double(CastsCutShort(p)); }, Num},
				{"Damage taken /s", [](const Player& p) { return PerS(p, double(p.DamageTaken)); }, Num},
			};
			std::vector<MeasureRow> rows;
			for (const Def& d : defs)
			{
				MeasureRow r;
				r.Name = d.Name;
				r.You = d.Value(c.You);
				std::vector<double> all{r.You};
				for (const Player& p : c.Peers) { all.push_back(d.Value(p)); }
				r.Median = Median(all);
				r.Best = r.Median;
				if (r.You <= 0 && r.Median <= 0) { continue; }
				r.YouText = d.Fmt(r.You);
				r.RefText = d.Fmt(r.Median);
				if (r.You <= r.Median) { r.Gap = r.You < r.Median ? "less than most" : "like the others"; }
				else if (d.Name == "Crowd controlled") { r.Gap = std::to_string(c.You.CcNoStab) + " with no stability just before"; }
				else if ((d.Name == "Time downed" || d.Name == "Time dead") && c.OneRound && c.MeRaw && !c.MeRaw->DownSpans.empty())
				{
					int32_t first = c.MeRaw->DownSpans[0].From;
					r.Gap = "first at " + Duration(first) + (InEnemySpike(*c.F, first) ? ", in an enemy spike" : "");
				}
				else { r.Gap = "more than most"; }
				rows.push_back(r);
			}
			return rows;
		}

		// One bullet row: track = best (or median), blue bar = you, tick = median
		bool BulletRow(int aIndex, const MeasureRow& r, bool aClickable)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			float lh = ImGui::GetTextLineHeight();
			float h = lh + 6;
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::PushID(aIndex);
			bool clicked = ImGui::Selectable("##m", false, aClickable ? 0 : ImGuiSelectableFlags_Disabled, ImVec2(0, h));
			ImGui::PopID();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			float ty = p.y + 3;
			dl->AddText(ImVec2(p.x + 4, ty), ink, r.Name.c_str());
			const float bx = p.x + 220, bw = 280, bh = lh * 0.45f;
			double mx = std::max({r.You, r.Best, r.Median});
			if (mx <= 0) { mx = 1; }
			float by = p.y + (h - bh) * 0.5f;
			if (!r.Unknown)
			{
				Rect(dl, ImVec2(bx, by), static_cast<float>(bw * r.Best / mx), bh, kTrack);
				Rect(dl, ImVec2(bx, by), static_cast<float>(bw * r.You / mx), bh, kYou);
			}
			float tx = bx + static_cast<float>(bw * r.Median / mx);
			dl->AddRectFilled(ImVec2(tx - 1, p.y + 2), ImVec2(tx + 1, p.y + h - 2), kPeerTick);
			auto right = [&](float aX, const std::string& aText, ImU32 aCol)
			{
				dl->AddText(ImVec2(aX - ImGui::CalcTextSize(aText.c_str()).x, ty), aCol, aText.c_str());
			};
			right(p.x + 590, r.YouText, ink);
			right(p.x + 680, r.RefText, muted);
			dl->AddText(ImVec2(p.x + 700, ty), ink, r.Gap.c_str());
			if (aClickable) { ImGui::RenderArrow(dl, ImVec2(p.x + ImGui::GetContentRegionAvail().x - 14, ty), muted, ImGuiDir_Right, 0.7f); }
			return clicked;
		}

		void MeasureHeader(const char* aScale, const char* aRef)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImU32 muted = ImGui::GetColorU32(kMuted);
			dl->AddText(ImVec2(p.x + 4, p.y), muted, "Measure");
			dl->AddText(ImVec2(p.x + 220, p.y), muted, aScale);
			dl->AddText(ImVec2(p.x + 590 - ImGui::CalcTextSize("You").x, p.y), muted, "You");
			dl->AddText(ImVec2(p.x + 680 - ImGui::CalcTextSize(aRef).x, p.y), muted, aRef);
			dl->AddText(ImVec2(p.x + 700, p.y), muted, "Gap");
			ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
		}

		// ---- the tile: your role's number, rank on your spec, rounds tonight -------------------------------------

		double RoleValue(const Ctx& c, const Player& p)
		{
			if (c.MyRole == R_Stab) { return p.StabEligible ? 100.0 * p.StabCovered / p.StabEligible : 0.0; }
			const Metric& m = Metrics()[c.RoleMetric];
			return Rate(m, p, m.Total(p));
		}

		std::string RoleText(const Ctx& c, double v) { return c.MyRole == R_Stab ? std::to_string(int(v + 0.5)) + "%" : Num(v); }

		const char* RoleLabel(const Ctx& c)
		{
			return c.MyRole == R_Heal ? "Healing /s" : c.MyRole == R_Stab ? "CC on your subgroup your stability covered" : "Damage to players /s";
		}

		void Tile(const Ctx& c)
		{
			float lh = ImGui::GetTextLineHeight();
			ImGui::BeginChild("tile", ImVec2(270, lh * 6.6f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			bool known = c.MyRole != R_Heal || c.You.HealKnown;
			double mine = RoleValue(c, c.You);
			ImGui::TextColored(kMuted, "%s, %s", RoleLabel(c), c.You.Spec.c_str());
			ImGui::SetWindowFontScale(1.8f);
			ImGui::TextUnformatted(known ? RoleText(c, mine).c_str() : "unknown");
			ImGui::SetWindowFontScale(1.0f);
			int rank = 1, of = 1;
			double best = 0;
			const Player* bestP = nullptr;
			for (const Player& p : c.Peers)
			{
				if (c.MyRole == R_Heal && !p.HealKnown) { continue; }
				double v = RoleValue(c, p);
				of++;
				if (v > mine) { rank++; }
				if (!bestP || v > best) { best = v; bestP = &p; }
			}
			static const char* kOrd[] = {"th", "st", "nd", "rd"};
			if (!known) { ImGui::TextColored(kMuted, "Healing Stats didn't report your heals"); }
			else if (bestP)
			{
				ImGui::TextColored(kMuted, "%d%s of %d %ss, best %s", rank, kOrd[(rank % 10 < 4 && (rank / 10) % 10 != 1) ? rank % 10 : 0], of,
					c.PeerLabel().c_str(), RoleText(c, best).c_str());
			}
			else { ImGui::TextColored(kMuted, "only %s", c.You.Spec.c_str()); }
			// Sparkline: your rounds tonight on this spec, this one in blue
			std::vector<std::pair<int, double>> pts;
			for (int i = 0; i < static_cast<int>(c.Fights->size()); i++)
			{
				const Fight& f = *(*c.Fights)[i];
				for (const Player& p : f.Players)
				{
					if (p.Account == c.You.Account && p.Spec == c.You.Spec && (c.MyRole != R_Heal || p.HealKnown)) { pts.push_back({i, RoleValue(c, p)}); }
				}
			}
			if (pts.size() >= 2)
			{
				ImDrawList* dl = ImGui::GetWindowDrawList();
				ImVec2 p = ImGui::GetCursorScreenPos();
				float w = 120, h = lh * 1.2f;
				double lo = pts[0].second, hi = pts[0].second;
				for (auto& [i, v] : pts) { lo = std::min(lo, v); hi = std::max(hi, v); }
				std::vector<ImVec2> line;
				ImVec2 current(-1, -1);
				for (size_t n = 0; n < pts.size(); n++)
				{
					float x = p.x + 4 + w * n / (pts.size() - 1);
					float y = p.y + h - 3 - static_cast<float>((pts[n].second - lo) / (hi > lo ? hi - lo : 1)) * (h - 6);
					line.push_back(ImVec2(x, y));
					if (pts[n].first == c.Index) { current = ImVec2(x, y); }
				}
				dl->AddPolyline(line.data(), static_cast<int>(line.size()), kPeerTick, false, 2.0f);
				if (current.x >= 0) { dl->AddCircleFilled(current, 4.0f, kYou); }
				ImGui::Dummy(ImVec2(w + 8, h));
				ImGui::SameLine();
				ImGui::TextColored(kMuted, "tonight");
			}
			ImGui::EndChild();
		}

		// One Back button that says where it goes, then where you are
		void Crumbs(const Ctx& c)
		{
			State& s = S();
			if (s.Measure == kNoMetric) { return; }
			const Metric& m = Metrics()[s.Measure];
			std::string measure = m.Name + (m.What == K_Boon ? "" : std::string(" ") + RateUnit(m));
			bool atSkill = s.Skill != kNoSkill;
			std::string back = "< Back to " + (atSkill ? measure : std::string("Review"));
			if (ImGui::Button(back.c_str())) { if (atSkill) { s.Skill = kNoSkill; } else { s.Measure = kNoMetric; } }
			ImGui::SameLine(0, 16);
			std::string path = "Review > " + measure + (atSkill ? " > " + c.Name(s.Skill) : std::string());
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(kMuted, "%s", path.c_str());
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
		}

		// A divider and a heading between the areas of the tab
		void Section(const char* aTitle)
		{
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextUnformatted(aTitle);
		}
	}

	void ReviewTab(const Ctx& c)
	{
		if (!c.MeRaw) { ImGui::TextColored(kMuted, "This log's recorder isn't in the squad, so there is no \"you\" to review."); return; }
		State& s = S();
		Crumbs(c);
		if (s.Measure != kNoMetric && s.Skill != kNoSkill) { SkillDetail(c, s.Measure, s.Skill); return; }
		if (s.Measure != kNoMetric) { SkillTable(c, s.Measure, true); return; }

		// The answer
		Tile(c);
		ImGui::SameLine();
		ImGui::BeginGroup();
		std::vector<Fix> fixes = FindFixes(c);
		const Metric& m = Metrics()[c.RoleMetric];
		if (!c.Vs) { Answer("Nobody else played your role this round, so there is nobody to compare with."); }
		else if (c.MyRole != R_Heal || (c.You.HealKnown && c.Vs->HealKnown))
		{
			double y = RoleValue(c, c.You), t = RoleValue(c, *c.Vs);
			std::string top;
			for (const Fix& f : fixes) { if (f.Kind == F_Skill) { top = c.Name(f.Skill); break; } }
			if (t > y)
			{
				std::string what = c.MyRole == R_Stab ? "covered " + RoleText(c, t) + " of their subgroup's CC, you " + RoleText(c, y)
					: std::string(c.MyRole == R_Heal ? "healed " : "did ") + Num(t - y) + " " + RateUnit(m) + " more";
				Answer(c.Vs->Name + " " + what + (top.empty() ? "." : ", mostly from " + top + "."));
			}
			else { Answer("You did the most of the " + c.PeerLabel() + "s: " + RoleText(c, y) + (c.MyRole == R_Stab ? "" : std::string(" ") + RateUnit(m)) + "."); }
		}
		else { Answer("Healing is unknown for one of you: it needs the Healing Stats addon running on that side."); }
		ImGui::TextColored(kMuted, "Click a line to see why, or a measure to see it by skill.");
		ImGui::EndGroup();

		// What to fix
		Section("What to fix, biggest first");
		if (c.Vs)
		{
			ImGui::SameLine(0, 30);
			Key(kYou, "you");
			Key(kPeer, c.Vs->Name.c_str());
			ImGui::NewLine();
		}
		if (fixes.empty()) { ImGui::TextUnformatted(c.Vs ? "Nothing stands out: you're level with them." : "-"); }
		for (int i = 0; i < static_cast<int>(fixes.size()); i++)
		{
			const Fix& f = fixes[i];
			bool open = s.OpenFix == i;
			if (FixLine(i, f, open, Recurring(c, f.Key))) { s.OpenFix = open ? -1 : i; open = !open; }
			if (!open) { continue; }
			ImGui::Indent(32);
			ImGui::Spacing();
			switch (f.Kind)
			{
			case F_Skill: SkillDetail(c, f.Metric, f.Skill); break;
			case F_Measure: SkillTable(c, f.Metric, true); break;
			case F_Cc:
				Answer("You were crowd controlled " + std::to_string(c.You.CcTaken) + " times for " + Num(c.You.CcTakenMs / 1000.0) + " s; " +
					std::to_string(c.You.CcNoStab) + " came with no stability on you in the 3 s before. " + c.Vs->Name + ": " +
					std::to_string(c.Vs->CcTaken) + " times.");
				if (ImGui::SmallButton("Show on the Fight time line")) { s.SwitchTo = T_Fight; }
				break;
			case F_Down:
				if (c.OneRound)
				{
					for (const Analysis::Span& sp : c.MeRaw->DownSpans)
					{
						ImGui::Text("%s at %s for %s s%s", sp.Dead ? "Dead" : "Downed", Duration(sp.From).c_str(), Num((sp.To - sp.From) / 1000.0).c_str(),
							InEnemySpike(*c.F, sp.From) ? ", in an enemy spike" : "");
					}
				}
				else { ImGui::Text("Downed %d times and dead %d times tonight.", c.You.Downs, c.You.Deaths); }
				if (ImGui::SmallButton("Show on the Fight time line")) { s.SwitchTo = T_Fight; }
				break;
			}
			ImGui::Spacing();
			ImGui::Unindent(32);
		}

		// Your measures
		Section(("Your measures against the other " + c.PeerLabel() + "s").c_str());
		ImGui::SameLine(0, 30);
		Key(kYou, "you");
		Key(kTrack, ("best " + c.PeerLabel()).c_str());
		Key(kPeerTick, "median");
		ImGui::NewLine();
		MeasureHeader("You against the best", ("Best " + c.PeerLabel()).c_str());
		std::vector<MeasureRow> rows = OutputRows(c);
		for (int i = 0; i < static_cast<int>(rows.size()); i++)
		{
			bool clickable = rows[i].Metric != kNoMetric && !rows[i].Unknown;
			if (BulletRow(i, rows[i], clickable) && clickable) { s.Measure = rows[i].Metric; s.Skill = kNoSkill; }
		}
		Section("What held you back, fewer is better");
		MeasureHeader("You against the median", "Median");
		std::vector<MeasureRow> held = HeldBackRows(c);
		for (int i = 0; i < static_cast<int>(held.size()); i++)
		{
			if (BulletRow(100 + i, held[i], true)) { s.SwitchTo = T_Fight; }
		}
	}
}
