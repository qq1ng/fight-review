// You tab: who you're compared with, a card for each of your spec's jobs (your number, the best, your rank), what
// to fix first (each line opens in place), and what held you back. A card opens its measure by skill (You >
// Healing /s), a skill the skill detail. The Tonight tab follows the same measures and fixes over the night.
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "SkillIcons.h"
#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		constexpr int kNoMetric = -1;

		// Where a measure stands among your main jobs: 0 = first, -1 = not one
		int JobRank(const std::vector<std::string>& aJobs, const std::string& aMeasure)
		{
			for (size_t i = 0; i < aJobs.size(); i++) { if (aMeasure == aJobs[i]) { return static_cast<int>(i); } }
			return -1;
		}

		// The measures that exist only as jobs (no skill table behind them)
		const char* const kJobCc = "CC on enemies /min";
		const char* const kJobRevives = "Rounds with a revive skill";
		const char* const kJobNegated = "Attacks negated /min";
		const char* const kJobDistortion = "Distortion timed with enemy spikes";

		enum FixKind { F_Skill, F_Measure, F_Cc, F_Down };

		struct Fix
		{
			double      Score = 0;
			std::string Key;              // stable across rounds, for "N of M rounds"
			FixKind     Kind = F_Skill;
			int32_t     Skill = kNoSkill;
			int         Metric = kNoMetric;
			std::string Subject, What, Unit, YouText, ThemText;
			std::string Detail;           // a measure with no skill table: the sentence shown when opened
			std::string Note;             // after the numbers: where most of their lead came from, if not on your bar
			double      You = 0, Them = 0;
		};

		bool InEnemySpike(const Fight& f, int32_t aMs)
		{
			for (int64_t t : f.TheirSpikesMs) { if (aMs >= t - 1000 && aMs <= t + 4000) { return true; } }
			return false;
		}

		// Rounds in scope a player played on this spec (at least 1)
		int RoundsOn(const Ctx& c, const Player& p)
		{
			auto [scope, account] = Lookup(c, p);
			int n = 0;
			for (const FightPtr& f : scope) { for (const Player& q : f->Players) { n += q.Account == account && q.Spec == p.Spec; } }
			return std::max(1, n);
		}

		// The few places where you fell behind the compared player, biggest first
		std::vector<Fix> FindFixes(const Ctx& c)
		{
			std::vector<Fix> out;
			if (!c.MeRaw || !c.Vs) { return out; }
			const Player& you = c.You;
			const Player& vs = *c.Vs;
			const Metric& m = Metrics()[c.RoleMetric];

			// Skills of your role's main output. Only against your own spec: a Specter compared with a Druid (nobody
			// else on Specter) would be told to cast Rejuvenating Tides (the user, 2026-09-24). Across specs the role's
			// total is the fix instead.
			if (!c.SameSpec && Known(m, you) && Known(m, vs))
			{
				double y = Rate(m, you, m.Total(you)), t = Rate(m, vs, m.Total(vs));
				if (t > 0 && (t - y) / t >= 0.10)
				{
					Fix f;
					f.Kind = F_Measure; f.Metric = c.RoleMetric; f.Key = "m:" + m.Name; f.Subject = m.Name;
					f.What = y <= 0 ? "none" : "lower";
					f.You = y; f.Them = t; f.YouText = Num(y); f.ThemText = Num(t); f.Unit = RateUnit(m);
					f.Score = (t - y) / t;
					out.push_back(f);
				}
			}
			if (c.SameSpec && Known(m, you) && Known(m, vs))
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
					const SkillRow* rowY = Row(you, s);
					const SkillRow* rowT = Row(vs, s);
					int castsY = rowY ? rowY->Casts : 0, castsT = rowT ? rowT->Casts : 0;
					// a skill you never used tonight on this spec isn't on your bar: a build choice, not a fix (the user,
					// 2026-09-25: Tale of the Honorable Rogue "not used" when it wasn't equipped), unless their lead comes
					// from it: a quarter of their output or more, with you 10%+ behind (the user, 2026-09-26)
					bool offBar = castsY == 0 && !c.UsedTonight.count(s);
					if (offBar)
					{
						double y0 = Rate(m, you, m.Total(you));
						if (w.Gap < 0.25 * total || (total - y0) / total < 0.10) { continue; }
					}
					// one cast a round can go before the fight (Power Break charged up out of combat, so its stability
					// reaches into the round): fewer casts counts only past one a round
					if (w.Word == "fewer casts" && castsT / double(RoundsOn(c, vs)) - castsY / double(RoundsOn(c, you)) <= 1.0) { continue; }
					Fix f;
					f.Score = w.Gap / total * (w.NoCasts ? 0.6 : 1.0);
					f.Key = "s:" + std::to_string(s);
					f.Skill = s; f.Metric = c.RoleMetric; f.Subject = c.Name(s); f.What = offBar ? "not on your bar" : w.Word;
					// a boon's own ticks (Regeneration's healing) read like the boon measure: say what it is
					for (const char* boon : Analysis::kBoonNames) { if (f.Subject == boon) { f.Subject = m.Name + " from " + f.Subject; } }
					f.You = w.You; f.Them = w.Them; f.YouText = w.YouText; f.ThemText = w.ThemText; f.Unit = w.Unit;
					out.push_back(f);
				}
			}

			// Other measures where the compared player clearly did more
			// A measure counts fully when it's one of your spec's main jobs (more for the first ones); anything else only
			// comes up when little else does (a Druid isn't told to strip)
			auto jobWeight = [&](int aMetric)
			{
				std::string job = aMetric == kMetricCleanses ? "Cleanses /min" : aMetric == kMetricStrips ? "Strips /min"
					: aMetric >= kMetricGroupBoon ? std::string(Analysis::kBoonNames[aMetric - kMetricGroupBoon]) + " on subgroup" : "";
				int rank = JobRank(c.Jobs, job);
				return rank < 0 ? (c.Jobs.empty() ? 1.0 : 0.15) : 1.2 - 0.15 * rank;
			};
			auto measure = [&](int aMetric, const std::string& aSubject, double y, double t, const std::string& aUnit, bool aEnough, double aWeight)
			{
				if (!aEnough || t <= 0) { return; }
				// against another spec, only your own jobs (a Druid's resolution isn't a Specter's job)
				if (!c.SameSpec && !c.Jobs.empty() && jobWeight(aMetric) < 0.2) { return; }
				aWeight *= jobWeight(aMetric);
				Fix f;
				f.Kind = F_Measure; f.Metric = aMetric; f.Key = "m:" + aSubject; f.Subject = aSubject;
				f.What = y <= 0 ? "none" : "lower";
				f.You = y; f.Them = t; f.YouText = Num(y); f.ThemText = Num(t); f.Unit = aUnit;
				f.Score = (t - y) / t * aWeight;
				// a quarter or more of theirs from a skill you never used tonight: say so (another build's source of it)
				if (aMetric >= 0 && aMetric < static_cast<int>(Metrics().size()))
				{
					const Metric& mm = Metrics()[aMetric];
					double all = mm.Total(vs), top = 0;
					int32_t topSkill = 0;
					for (auto& [sk, r] : vs.Skills)
					{
						double v = mm.PerSkill(r);
						if (sk != 0 && !c.UsedTonight.count(sk) && v > top) { top = v; topSkill = sk; }
					}
					if (topSkill && all > 0 && top >= 0.25 * all)
					{
						f.Note = std::to_string(int(100 * top / all + 0.5)) + "% of theirs from " + c.Name(topSkill) + ", not on your bar";
					}
				}
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
				double y = GroupGenOver(Lookup(c, you).first, Lookup(c, you).second, you.Spec, b), t = GroupGenOver(Lookup(c, vs).first, Lookup(c, vs).second, vs.Spec, b);
				bool stacks = c.F->Intensity[b];
				measure(kMetricGroupBoon + b, std::string(Analysis::kBoonNames[b]) + " on your subgroup", y, t, stacks ? "stacks" : "% uptime",
					t - y >= (stacks ? 0.3 : 8.0) && t >= 1.3 * y, 0.5);
			}
			if (you.StabEligible >= 3 && vs.StabEligible >= 3)
			{
				double y = 100.0 * you.StabCovered / you.StabEligible, t = 100.0 * vs.StabCovered / vs.StabEligible;
				measure(kMetricGroupBoon + Analysis::kStability, "CC covered by your stability", y, t, "% of CC on your subgroup", t - y >= 15, 0.9);
			}
			// Jobs with no skill table (CC landed, revive skills, distortion timing): only when they're your jobs
			auto jobMeasure = [&](const char* aJob, double y, double t, const std::string& aUnit, bool aEnough, const std::string& aDetail)
			{
				int rank = JobRank(c.Jobs, aJob);
				if (rank < 0 || !aEnough || t <= 0) { return; }
				Fix f;
				f.Kind = F_Measure; f.Key = std::string("m:") + aJob; f.Subject = aJob; f.Detail = aDetail;
				f.What = y <= 0 ? "none" : "lower";
				f.You = y; f.Them = t; f.YouText = Num(y); f.ThemText = Num(t); f.Unit = aUnit;
				f.Score = (t - y) / t * 0.6 * (1.2 - 0.15 * rank);
				out.push_back(f);
			};
			{
				double y = PerS(you, you.CcDealt) * 60, t = PerS(vs, vs.CcDealt) * 60;
				jobMeasure(kJobCc, y, t, "per minute", t - y >= 1.0 && t >= 1.3 * y,
					"You landed " + std::to_string(you.CcDealt) + " CC on enemies (" + Num(y) + " a minute), " + vs.Name + " " + std::to_string(vs.CcDealt) + " (" + Num(t) + ").");
			}
			{
				auto [wy, py] = RoundsWithRevive(Lookup(c, you).first, Lookup(c, you).second, you.Spec);
				auto [wt, pt] = RoundsWithRevive(Lookup(c, vs).first, Lookup(c, vs).second, vs.Spec);
				double y = py ? 100.0 * wy / py : 0, t = pt ? 100.0 * wt / pt : 0;
				jobMeasure(kJobRevives, y, t, "% of rounds", py && pt && t - y >= 25,
					"Your revive skill went off with an ally down in reach in " + std::to_string(wy) + " of " + std::to_string(py) + " rounds; " +
					vs.Name + "'s in " + std::to_string(wt) + " of " + std::to_string(pt) + ".");
			}
			{
				double y = PerS(you, you.Evades + you.Blocks + you.Invulns) * 60, t = PerS(vs, vs.Evades + vs.Blocks + vs.Invulns) * 60;
				jobMeasure(kJobNegated, y, t, "per minute", t - y >= 3 && t >= 1.25 * y,
					"You negated " + std::to_string(you.Evades + you.Blocks + you.Invulns) + " attacks (evaded, blocked or invulnerable), " +
					vs.Name + " " + std::to_string(vs.Evades + vs.Blocks + vs.Invulns) + ".");
			}
			{
				auto [ty, ay] = CastsOnEnemySpikes(Lookup(c, you).first, Lookup(c, you).second, you.Spec, kTaleOfTheAugustQueen);
				auto [tt, at] = CastsOnEnemySpikes(Lookup(c, vs).first, Lookup(c, vs).second, vs.Spec, kTaleOfTheAugustQueen);
				double y = ay ? 100.0 * ty / ay : 0, t = at ? 100.0 * tt / at : 0;
				jobMeasure(kJobDistortion, y, t, "% of Tale of the August Queen casts", at >= 2 && t - y >= 20,
					std::to_string(ty) + " of your " + std::to_string(ay) + " Tale of the August Queen casts went off from 3 s before an enemy spike to 1 s into it; " +
					vs.Name + ": " + std::to_string(tt) + " of " + std::to_string(at) + ".");
			}

			// What held you back
			if (you.CcTaken >= vs.CcTaken + 2 && you.CcTaken >= 1.5 * vs.CcTaken)
			{
				Fix f;
				f.Kind = F_Cc; f.Key = "cc"; f.Subject = "Crowd controlled";
				f.What = vs.CcTaken ? std::to_string(int(std::lround(double(you.CcTaken) / vs.CcTaken))) + "x as often" : "more often";
				f.You = you.CcTaken; f.Them = vs.CcTaken; f.YouText = std::to_string(you.CcTaken); f.ThemText = std::to_string(vs.CcTaken);
				f.Unit = "times"; // the opened line says how many came with no stability
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
			// Your role's number comes first: when it's 10%+ below, its two biggest skill gaps lead the list
			double y = Rate(m, you, m.Total(you)), t = Rate(m, vs, m.Total(vs));
			if (Known(m, you) && Known(m, vs) && t > 0 && (t - y) / t >= 0.10)
			{
				std::vector<Fix> lead, rest;
				for (const Fix& f : out) { (f.Kind == F_Skill && f.Metric == c.RoleMetric && lead.size() < 2 ? lead : rest).push_back(f); }
				lead.insert(lead.end(), rest.begin(), rest.end());
				out.swap(lead);
			}
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
			float h = lh * (f.Note.empty() ? 1.7f : 2.5f); // a note gets a line of its own
			ImVec2 p = ImGui::GetCursorScreenPos();
			float width = ImGui::GetContentRegionAvail().x;
			ImGui::PushID(aIndex);
			bool clicked = ImGui::Selectable("##fix", aOpen, 0, ImVec2(0, h));
			ImGui::PopID();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			float ty = p.y + (lh * 1.7f - lh) * 0.5f;
			ImGui::RenderArrow(dl, ImVec2(p.x + 2, ty), muted, aOpen ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
			dl->AddText(ImVec2(p.x + 18, ty), muted, std::to_string(aIndex + 1).c_str());
			float x = p.x + 32;
			if (f.Kind == F_Skill)
			{
				if (void* icon = SkillIcons::Get(f.Skill, f.Subject)) { dl->AddImage(icon, ImVec2(x, ty), ImVec2(x + lh, ty + lh)); }
				x += lh + 5;
			}
			// the name, cut to fit before the next column (the full name on hover)
			std::string subject = f.Subject;
			float room = p.x + 242 - x;
			if (ImGui::CalcTextSize(subject.c_str()).x > room)
			{
				while (!subject.empty() && ImGui::CalcTextSize((subject + "...").c_str()).x > room) { subject.pop_back(); }
				subject += "...";
				if (ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x < p.x + 242) { ImGui::SetTooltip("%s", f.Subject.c_str()); }
			}
			dl->AddText(ImVec2(x, ty), ink, subject.c_str());
			dl->AddText(ImVec2(p.x + 250, ty), ink, f.What.c_str());
			PairBars(dl, ImVec2(p.x + 380, p.y), 130, lh * 1.7f, f);
			std::string values = f.YouText + " / " + f.ThemText;
			dl->AddText(ImVec2(p.x + 520, ty), ink, values.c_str());
			dl->AddText(ImVec2(p.x + 528 + ImGui::CalcTextSize(values.c_str()).x, ty), muted, f.Unit.c_str());
			if (!f.Note.empty())
			{
				// on its own line under the numbers, so the tag at the right keeps its room
				SmallText(dl, ImVec2(p.x + 528, ty + ImGui::GetTextLineHeight() * 0.95f), muted, f.Note);
			}
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
			int Rank = 1, Of = 1;       // your place among your peers on it
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
			if (c.MyRole == R_Damage || c.MyRole == R_Strip)
			{
				defs.push_back({"Damage to all /s", kMetricDamageAll, rate(kMetricDamageAll), Num, 100, false});
				defs.push_back({"Damage in our spikes", kMetricDamage, [&c](const Player& p) { return std::max(0.0, SpikeShare(Lookup(c, p).first, Lookup(c, p).second, p.Spec)); },
					[](double v) { return std::to_string(int(v + 0.5)) + "%"; }, 1, false});
			}
			defs.push_back({kJobCc, kNoMetric, [](const Player& p) { return PerS(p, p.CcDealt) * 60; }, Num, 0.5, false});
			if (JobRank(c.Jobs, kJobRevives) >= 0)
			{
				defs.push_back({kJobRevives, kNoMetric, [&c](const Player& p)
					{
						auto [with, played] = RoundsWithRevive(Lookup(c, p).first, Lookup(c, p).second, p.Spec);
						return played ? 100.0 * with / played : 0.0;
					},
					[](double v) { return std::to_string(int(v + 0.5)) + "%"; }, 1, false});
			}
			if (JobRank(c.Jobs, kJobNegated) >= 0)
			{
				defs.push_back({kJobNegated, kNoMetric, [](const Player& p) { return PerS(p, p.Evades + p.Blocks + p.Invulns) * 60; }, Num, 1, false});
			}
			if (JobRank(c.Jobs, kJobDistortion) >= 0)
			{
				defs.push_back({kJobDistortion, kNoMetric, [&c](const Player& p)
					{
						auto [timed, all] = CastsOnEnemySpikes(Lookup(c, p).first, Lookup(c, p).second, p.Spec, kTaleOfTheAugustQueen);
						return all ? 100.0 * timed / all : 0.0;
					},
					[](double v) { return std::to_string(int(v + 0.5)) + "%"; }, 1, false});
			}
			if (c.You.StabEligible >= 3)
			{
				defs.push_back({"CC covered by your stability", kMetricGroupBoon + Analysis::kStability,
					[](const Player& p) { return p.StabEligible >= 3 ? 100.0 * p.StabCovered / p.StabEligible : 0.0; },
					[](double v) { return std::to_string(int(v + 0.5)) + "%"; }, 1, false});
			}
			for (int b = 0; b < Analysis::kBoons; b++)
			{
				bool stacks = c.F->Intensity[b];
				defs.push_back({std::string(Analysis::kBoonNames[b]) + " on subgroup", kMetricGroupBoon + b,
					[&c, b](const Player& p) { return GroupGenOver(Lookup(c, p).first, Lookup(c, p).second, p.Spec, b); },
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
					r.Of++;
					r.Rank += v > r.You;
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
			int lead = c.RoleMetric;
			const char* leadName = c.MyRole == R_Stab ? "CC covered by your stability" : nullptr;
			auto isLead = [&](const MeasureRow& r) { return leadName ? r.Name == leadName : r.Metric == lead; };
			// then your spec's main jobs (what it does most in these logs), then the rest by gap
			auto rank = [&](const MeasureRow& r) { if (isLead(r)) { return -1; } int j = JobRank(c.Jobs, r.Name); return j < 0 ? 100 : j; };
			std::stable_sort(rows.begin(), rows.end(), [&](const MeasureRow& a, const MeasureRow& b)
			{
				if (rank(a) != rank(b)) { return rank(a) < rank(b); }
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

		// ---- your role's number (the summary window), the job cards, who you're compared with -------------------

		double RoleValue(const Ctx& c, const Player& p)
		{
			if (c.MyRole == R_Stab) { return p.StabEligible ? 100.0 * p.StabCovered / p.StabEligible : 0.0; }
			const Metric& m = Metrics()[c.RoleMetric];
			return Rate(m, p, m.Total(p));
		}

		std::string RoleText(const Ctx& c, double v) { return c.MyRole == R_Stab ? std::to_string(int(v + 0.5)) + "%" : Num(v); }

		const char* RoleLabel(const Ctx& c)
		{
			return c.MyRole == R_Heal ? "Healing /s" : c.MyRole == R_Stab ? "CC on your subgroup your stability covered" : c.MyRole == R_Strip ? "Strips /min" : "Damage to players /s";
		}

		const char* RoleWord(const Ctx& c)
		{
			return c.MyRole == R_Heal ? "healer" : c.MyRole == R_Stab ? "stability" : c.MyRole == R_Strip ? "strips" : "damage";
		}

		std::string Ordinal(int n)
		{
			static const char* kOrd[] = {"th", "st", "nd", "rd"};
			return std::to_string(n) + kOrd[(n % 10 < 4 && (n / 10) % 10 != 1) ? n % 10 : 0];
		}

		// "Specter, healer: your 6 jobs against Mira Coe (best Specter this round)  [Change]"
		void VsLine(const Ctx& c, int aJobs)
		{
			State& s = S();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%s, %s", c.You.Spec.c_str(), RoleWord(c));
			ImGui::SameLine();
			std::string jobs = aJobs > 0 ? "your " + std::to_string(aJobs) + (aJobs == 1 ? " job" : " jobs") + " against" : "against";
			ImGui::TextColored(kMuted, "%s", jobs.c_str());
			ImGui::SameLine();
			if (!c.Vs) { ImGui::TextUnformatted("nobody: nobody else played your role this round"); return; }
			ImGui::TextUnformatted(c.Vs->Name.c_str());
			ImGui::SameLine();
			std::string why = c.BestOwnRound >= 0 && c.Vs == &c.Peers.back() ? "(your best round tonight on " + c.You.Spec + "; nobody else played it this round)"
				: s.VsAccount.empty() ? "(best " + c.PeerLabel() + (c.OneRound ? " this round" : " tonight") + ")" : "(your pick, " + c.Vs->Spec + ")";
			if (!c.SameSpec) { why += "; nobody else played " + c.You.Spec + ", so totals only"; }
			ImGui::TextColored(kMuted, "%s", why.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("Change")) { ImGui::OpenPopup("vs"); }
			if (ImGui::BeginPopup("vs"))
			{
				const Metric& m = Metrics()[c.RoleMetric];
				if (ImGui::Selectable(("The best " + c.PeerLabel()).c_str(), s.VsAccount.empty())) { s.VsAccount.clear(); }
				for (const Player& p : c.Peers)
				{
					std::string rate = Known(m, p) ? Num(Rate(m, p, m.Total(p))) + " " + RateUnit(m) : std::string("unknown");
					std::string item = p.Name + (c.SameSpec ? "" : " (" + p.Spec + ")") + "   " + m.Name + " " + rate + "##" + p.Account;
					if (ImGui::Selectable(item.c_str(), &p == c.Vs && !s.VsAccount.empty())) { s.VsAccount = p.Account; }
				}
				ImGui::EndPopup();
			}
		}

		// A card per job, three to a row: number and name, your value, the best, your rank, a bar with the best's tick
		// Returns the measure clicked (its metric), or kNoMetric
		int JobCards(const std::vector<MeasureRow>& aRows, int aCount)
		{
			int clicked = kNoMetric;
			float avail = ImGui::GetContentRegionAvail().x, lh = ImGui::GetTextLineHeight();
			const float gap = 6, w = (avail - 2 * gap) / 3, h = lh * 4.1f;
			for (int i = 0; i < aCount; i++)
			{
				const MeasureRow& r = aRows[i];
				if (i % 3) { ImGui::SameLine(0, gap); }
				ImGui::PushID(i);
				ImVec2 p = ImGui::GetCursorScreenPos();
				bool clickable = r.Metric != kNoMetric && !r.Unknown;
				if (ImGui::InvisibleButton("card", ImVec2(w, h)) && clickable) { clicked = r.Metric; }
				bool hovered = ImGui::IsItemHovered();
				if (hovered && clickable) { ImGui::SetTooltip("By skill"); }
				ImGui::PopID();
				ImDrawList* dl = ImGui::GetWindowDrawList();
				ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
				dl->AddRect(p, ImVec2(p.x + w, p.y + h), hovered && clickable ? ImGui::GetColorU32(ImGuiCol_ButtonHovered) : IM_COL32(0x3b, 0x42, 0x50, 255));
				float x0 = p.x + 8, y0 = p.y + 5;
				std::string title = std::to_string(i + 1) + ". " + r.Name;
				dl->PushClipRect(p, ImVec2(p.x + w - 70, p.y + h), true);
				dl->AddText(ImVec2(x0, y0), muted, title.c_str());
				dl->PopClipRect();
				std::string rank = r.Unknown || r.Of <= 1 ? std::string() : Ordinal(r.Rank) + " of " + std::to_string(r.Of);
				dl->AddText(ImVec2(p.x + w - 8 - ImGui::CalcTextSize(rank.c_str()).x, y0), muted, rank.c_str());
				float vy = y0 + lh + 2;
				dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.5f, ImVec2(x0, vy), ink, r.YouText.c_str());
				float vw = ImGui::CalcTextSize(r.YouText.c_str()).x * 1.5f;
				std::string best = "best " + r.RefText;
				dl->AddText(ImVec2(x0 + vw + 10, vy + lh * 0.4f), muted, best.c_str());
				std::string gapText = r.Unknown ? std::string() : r.You >= r.Best ? "the best" : r.Gap;
				dl->AddText(ImVec2(p.x + w - 8 - ImGui::CalcTextSize(gapText.c_str()).x, vy + lh * 0.4f), r.You >= r.Best ? kYou : ink, gapText.c_str());
				// the bar: yours in blue over a track, the best as a light tick
				float by = p.y + h - 10, bw = w - 16;
				double mx = std::max({r.You, r.Best, 1e-9});
				dl->AddRectFilled(ImVec2(x0, by), ImVec2(x0 + bw, by + 5), kTrack);
				if (!r.Unknown) { dl->AddRectFilled(ImVec2(x0, by), ImVec2(x0 + static_cast<float>(bw * r.You / mx), by + 5), kYou); }
				float tx = x0 + static_cast<float>(bw * r.Best / mx) - 1;
				dl->AddRectFilled(ImVec2(tx, by - 3), ImVec2(tx + 2, by + 8), kPeerTick);
			}
			return clicked;
		}

		// One Back button that says where it goes, then where you are
		void Crumbs(const Ctx& c)
		{
			State& s = S();
			if (s.Measure == kNoMetric) { return; }
			const Metric& m = Metrics()[s.Measure];
			std::string measure = m.Name + (m.What == K_Boon ? "" : std::string(" ") + RateUnit(m));
			bool atSkill = s.Skill != kNoSkill;
			std::string back = "< Back to " + (atSkill ? measure : std::string("You"));
			if (ImGui::Button(back.c_str())) { if (atSkill) { s.Skill = kNoSkill; } else { s.Measure = kNoMetric; } }
			ImGui::SameLine(0, 16);
			std::string path = "You > " + measure + (atSkill ? " > " + c.Name(s.Skill) : std::string());
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
		if (!c.MeRaw) { ImGui::TextColored(kMuted, "%s", NoYou(*c.F).c_str()); return; }
		State& s = S();
		Crumbs(c);
		if (s.Measure != kNoMetric && s.Skill != kNoSkill) { SkillDetail(c, s.Measure, s.Skill); return; }
		if (s.Measure != kNoMetric) { SkillTable(c, s.Measure, true); return; }

		std::vector<MeasureRow> rows = OutputRows(c);
		std::vector<Fix> fixes = FindFixes(c);
		// Your role's measure and your spec's jobs as cards; the rest on request
		int main = 0; // rows are sorted: your role's measure first, then your spec's jobs, then the rest
		for (size_t i = 0; i < rows.size(); i++) { if (i == 0 || JobRank(c.Jobs, rows[i].Name) >= 0) { main = static_cast<int>(i) + 1; } else { break; } }
		main = std::max(main, std::min<int>(3, static_cast<int>(rows.size())));
		VsLine(c, c.Jobs.empty() ? 0 : main);
		if (c.MyRole == R_Heal && (!c.You.HealKnown || (c.Vs && !c.Vs->HealKnown)))
		{
			ImGui::TextColored(kMuted, "Healing is unknown for %s: it needs the Healing Stats addon running on that side.", !c.You.HealKnown ? "you" : c.Vs->Name.c_str());
		}
		int count = s.AllMeasures ? static_cast<int>(rows.size()) : main;
		int open = JobCards(rows, count);
		if (open != kNoMetric) { s.Measure = open; s.Skill = kNoSkill; }
		if (static_cast<int>(rows.size()) > main)
		{
			std::string more = s.AllMeasures ? "Show only your jobs" : "Show " + std::to_string(rows.size() - main) + " more measures";
			if (ImGui::SmallButton(more.c_str())) { s.AllMeasures = !s.AllMeasures; }
			if (!s.AllMeasures)
			{
				std::string names;
				for (size_t i = main; i < rows.size(); i++) { names += (names.empty() ? "" : ", ") + Lower(rows[i].Name); }
				ImGui::SameLine();
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextColored(kMuted, "%s: not %s jobs, so they don't make fixes", names.c_str(), c.You.Spec.c_str());
				ImGui::PopTextWrapPos();
			}
		}

		// What to fix
		Section("Fix first");
		if (c.Vs)
		{
			ImGui::SameLine(0, 30);
			Key(kYou, "you");
			Key(kPeer, c.Vs->Name.c_str());
			ImGui::NewLine();
		}
		if (fixes.empty()) { ImGui::TextUnformatted(c.Vs ? "Nothing stands out: you're level with them." : "-"); }
		int shown = s.MoreFixes ? static_cast<int>(fixes.size()) : std::min<int>(3, static_cast<int>(fixes.size()));
		for (int i = 0; i < shown; i++)
		{
			const Fix& f = fixes[i];
			bool isOpen = s.OpenFix == i;
			if (FixLine(i, f, isOpen, Recurring(c, f.Key))) { s.OpenFix = isOpen ? -1 : i; isOpen = !isOpen; }
			if (!isOpen) { continue; }
			ImGui::Indent(32);
			ImGui::Spacing();
			switch (f.Kind)
			{
			case F_Skill: SkillDetail(c, f.Metric, f.Skill); break;
			case F_Measure: if (f.Metric == kNoMetric) { Answer(f.Detail); } else { SkillTable(c, f.Metric, true); } break;
			case F_Cc:
				Answer("You were crowd controlled " + std::to_string(c.You.CcTaken) + " times for " + Num(c.You.CcTakenMs / 1000.0) + " s; " +
					std::to_string(c.You.CcNoStab) + " came with no stability on you in the 3 s before. " + c.Vs->Name + ": " +
					std::to_string(c.Vs->CcTaken) + " times.");
				break;
			case F_Down:
				if (c.OneRound)
				{
					for (const Analysis::Span& sp : c.MeRaw->DownSpans)
					{
						if (sp.Dead) { ImGui::Text("Dead at %s for %s s", Duration(sp.From).c_str(), Num((sp.To - sp.From) / 1000.0).c_str()); continue; }
						Answer("Downed at " + Duration(sp.From) + " for " + Num((sp.To - sp.From) / 1000.0) + " s: " + CauseOf(*c.F, *c.MeRaw, sp).Line);
					}
				}
				else { ImGui::Text("Downed %d times and dead %d times tonight.", c.You.Downs, c.You.Deaths); }
				if (ImGui::SmallButton("Open in Deaths")) { s.SwitchTo = T_Deaths; s.DeathFilter = 1; s.DeathKey.clear(); s.DeathSpike = -1; }
				break;
			}
			ImGui::Spacing();
			ImGui::Unindent(32);
		}
		if (static_cast<int>(fixes.size()) > 3)
		{
			std::string more = s.MoreFixes ? "Show fewer" : "Show " + std::to_string(fixes.size() - 3) + " more";
			if (ImGui::SmallButton(more.c_str())) { s.MoreFixes = !s.MoreFixes; }
		}

		// What held you back: only where you had more of it than most
		std::vector<MeasureRow> held = HeldBackRows(c);
		held.erase(std::remove_if(held.begin(), held.end(), [](const MeasureRow& r) { return r.You <= r.Median; }), held.end());
		Section("What held you back, fewer is better");
		if (held.empty()) { ImGui::TextColored(kMuted, "Nothing: you were crowd controlled, downed and hit no more than most %ss.", c.PeerLabel().c_str()); }
		else
		{
			MeasureHeader("You against the median", "Median");
			for (int i = 0; i < static_cast<int>(held.size()); i++)
			{
				if (BulletRow(100 + i, held[i], true)) { s.SwitchTo = T_Deaths; s.DeathFilter = 1; s.DeathKey.clear(); s.DeathSpike = -1; }
			}
		}
	}

	namespace
	{
		// Tonight, round by round: your role's number and the best on your spec, who won, the fixes that came up
		struct NightRound
		{
			int Index = 0;
			bool Mine = false;       // you played this spec in it
			bool Short = false;      // under 30 s
			double Value = 0, Best = 0;
			bool Known = true;
			int Result = 0;          // 1 won, -1 lost, 0 even (downs each side)
			std::vector<Fix> Fixes;
		};
		struct Night
		{
			std::vector<NightRound> Rounds;
			int Played = 0;
		};

		const Night& NightOf(const Ctx& c)
		{
			static std::string cacheKey;
			static Night night;
			const auto& fights = *c.Fights;
			std::string key = std::to_string(fights.size()) + "|" + (fights.empty() ? "" : fights.back()->Stamp) + "|" + c.You.Account + "|" + c.You.Spec + "|" + S().VsAccount;
			if (key == cacheKey) { return night; }
			cacheKey = key;
			night = Night{};
			for (int i = 0; i < static_cast<int>(fights.size()); i++)
			{
				const Fight& f = *fights[i];
				NightRound r;
				r.Index = i;
				r.Short = f.DurationMs < 30000;
				r.Result = f.EnemyDowns > f.SquadDowns ? 1 : f.EnemyDowns < f.SquadDowns ? -1 : 0;
				const Player* me = nullptr;
				for (const Player& p : f.Players) { if (p.Account == c.You.Account && p.Spec == c.You.Spec) { me = &p; } }
				if (me)
				{
					Ctx ci = BuildCtx(fights, i, false, me->Account);
					r.Mine = true;
					r.Known = c.MyRole != R_Heal || me->HealKnown;
					r.Value = RoleValue(ci, ci.You);
					for (const Player& p : ci.Peers) { if (c.MyRole != R_Heal || p.HealKnown) { r.Best = std::max(r.Best, RoleValue(ci, p)); } }
					r.Fixes = FindFixes(ci);
					night.Played++;
				}
				night.Rounds.push_back(r);
			}
			return night;
		}
	}

	void TonightTab(const Ctx& c)
	{
		const auto& fights = *c.Fights;
		if (fights.empty()) { return; }
		if (!c.MeRaw) { ImGui::TextColored(kMuted, "This log's recorder isn't in the squad, so there is no \"you\" to follow over the night."); return; }
		const Night& n = NightOf(c);
		float lh = ImGui::GetTextLineHeight();

		// Which fixes kept coming back, over the night (the user's v5): in how many of your rounds, the reason it came up
		// most, your typical number against the one compared (medians over those rounds), and which rounds
		struct Recur
		{
			std::string Key, Subject, Latest, Unit;
			int Rounds = 0, Last = -1;
			std::map<std::string, int> Whats;  // the reason word -> rounds
			struct Seen1 { std::string What, Unit; double You, Them; };
			std::vector<Seen1> Each;            // each round it came up: its reason and numbers
			std::set<int> Seen;                 // round indices it came up in
		};
		std::vector<Recur> recur;
		std::vector<double> values;
		const NightRound* best = nullptr;
		const NightRound* worst = nullptr;
		int downs = 0, deaths = 0;
		for (const NightRound& r : n.Rounds)
		{
			if (!r.Mine) { continue; }
			for (const Fix& x : r.Fixes)
			{
				auto it = std::find_if(recur.begin(), recur.end(), [&](const Recur& q) { return q.Key == x.Key; });
				if (it == recur.end()) { recur.push_back({x.Key, x.Subject}); it = recur.end() - 1; }
				it->Rounds++;
				it->Last = r.Index;
				it->Latest = x.What + ": " + x.YouText + " vs " + x.ThemText + " " + x.Unit;
				it->Unit = x.Unit;
				it->Whats[x.What]++;
				it->Each.push_back({x.What, x.Unit, x.You, x.Them});
				it->Seen.insert(r.Index);
			}
			if (r.Short || !r.Known) { continue; }
			values.push_back(r.Value);
			if (!best || r.Value > best->Value) { best = &r; }
			if (!worst || r.Value < worst->Value) { worst = &r; }
		}
		for (const FightPtr& fp : fights) { for (const Player& p : fp->Players) { if (p.Account == c.You.Account) { downs += p.Downs; deaths += p.Deaths; } } }
		std::sort(recur.begin(), recur.end(), [](const Recur& a, const Recur& b) { return a.Rounds > b.Rounds; });
		std::sort(values.begin(), values.end());
		double median = values.empty() ? 0 : values[values.size() / 2];

		// The answer
		{
			std::string label = RoleLabel(c);
			std::string text = values.empty() ? "Not enough of your rounds tonight to say." :
				c.MyRole == R_Stab ? "Your stability covered about " + RoleText(c, median) + " of the CC on your subgroup in a typical round"
				: "Your " + Lower(label) + " was about " + RoleText(c, median) + " in a typical round";
			if (!values.empty())
			{
				if (!recur.empty() && recur[0].Rounds >= 2)
				{
					text += "; the fix that came back most: " + recur[0].Subject + ", in " + std::to_string(recur[0].Rounds) + " of " + std::to_string(n.Played) + " rounds.";
				}
				else { text += "; no fix came back more than once."; }
			}
			ImGui::SetWindowFontScale(1.1f);
			Answer(text);
			ImGui::SetWindowFontScale(1.0f);
		}

		// Your number per round, the best on your spec as a tick, who won under each round
		ImGui::Spacing();
		ImGui::Text("%s per round", RoleLabel(c));
		ImGui::SameLine(0, 16);
		Key(kYou, "you");
		ImVec2 kp = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(kp.x, kp.y + lh * 0.45f), ImVec2(kp.x + lh * 0.9f, kp.y + lh * 0.55f + 1), kPeerTick);
		ImGui::Dummy(ImVec2(lh * 0.9f, lh));
		ImGui::SameLine(0, 4);
		ImGui::TextColored(kMuted, "best %s that round", c.PeerLabel().c_str());
		ImGui::SameLine(0, 12);
		ImGui::TextColored(kMuted, "grey: under 30 s");
		float width = ImGui::GetContentRegionAvail().x, h = lh * 9;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 top = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("night", ImVec2(width, h + lh * 2.2f));
		bool hovered = ImGui::IsItemHovered();
		dl->AddRectFilled(top, ImVec2(top.x + width, top.y + h), kLaneBg);
		double mx = 1e-9;
		for (const NightRound& r : n.Rounds) { if (r.Mine && r.Known) { mx = std::max({mx, r.Value, r.Best}); } }
		int count = static_cast<int>(n.Rounds.size());
		float slot = width / std::max(1, count), bw = std::max(2.0f, slot * 0.7f);
		for (const NightRound& r : n.Rounds)
		{
			float x = top.x + slot * r.Index + (slot - bw) * 0.5f;
			if (r.Mine && r.Known)
			{
				float bh = static_cast<float>(r.Value / mx) * (h - 4);
				dl->AddRectFilled(ImVec2(x, top.y + h - bh), ImVec2(x + bw, top.y + h), r.Short ? IM_COL32(0x3b, 0x3f, 0x46, 255) : kYou);
				if (r.Best > 0)
				{
					float by = top.y + h - static_cast<float>(r.Best / mx) * (h - 4);
					dl->AddRectFilled(ImVec2(x - 1, by - 1), ImVec2(x + bw + 1, by + 1), kPeerTick);
				}
			}
			// the round's result: a strip under it (the colour), and the hover says it in words
			ImU32 res = r.Result > 0 ? kYou : r.Result < 0 ? kEnemy : kPeer;
			dl->AddRectFilled(ImVec2(x, top.y + h + 3), ImVec2(x + bw, top.y + h + 7), res);
			if (r.Index == c.Index) { dl->AddRect(ImVec2(x - 2, top.y), ImVec2(x + bw + 2, top.y + h + 8), ImGui::GetColorU32(ImGuiCol_Text)); }
			if (r.Index % 5 == 0 || r.Index == count - 1) { SmallText(dl, ImVec2(x, top.y + h + 9), ImGui::GetColorU32(kMuted), std::to_string(r.Index + 1)); }
		}
		if (hovered)
		{
			int i = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - top.x) / slot), 0, count - 1);
			const NightRound& r = n.Rounds[i];
			const Fight& f = *fights[i];
			ImGui::BeginTooltip();
			ImGui::Text("Round %d, %s, %s: %s, downed %d of theirs, %d of ours", i + 1, Clock(f.Stamp).c_str(), Duration(f.DurationMs).c_str(),
				r.Result > 0 ? "won" : r.Result < 0 ? "lost" : "even", f.EnemyDowns, f.SquadDowns);
			if (!r.Mine) { ImGui::TextColored(kMuted, "You weren't on %s this round.", c.You.Spec.c_str()); }
			else if (!r.Known) { ImGui::TextColored(kMuted, "Your healing is unknown this round."); }
			else { ImGui::Text("You %s, best %s %s", RoleText(c, r.Value).c_str(), c.PeerLabel().c_str(), r.Best > 0 ? RoleText(c, r.Best).c_str() : "-"); }
			ImGui::TextColored(kMuted, "Click to open it");
			ImGui::EndTooltip();
			if (ImGui::IsItemClicked()) { S().Selected = i == count - 1 ? -1 : i; S().SwitchTo = T_You; S().AllRounds = false; }
		}
		ImGui::TextColored(kMuted, "Under each round: blue we downed more of theirs, orange they downed more of ours, grey even.");

		// The fixes that keep coming back, across the window; under it, your rounds and the squad's night
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextUnformatted("Fixes that keep coming back");
		auto middle = [](std::vector<double> v) { if (v.empty()) { return 0.0; } std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
		if (recur.empty()) { ImGui::TextColored(kMuted, "None: nothing to fix came up tonight."); }
		else if (ImGui::BeginTable("recurring", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("Fix", ImGuiTableColumnFlags_WidthFixed, 200);
			ImGui::TableSetupColumn("Rounds", ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableSetupColumn("Most often", ImGuiTableColumnFlags_WidthFixed, 170);
			ImGui::TableSetupColumn("Typical: you vs them", ImGuiTableColumnFlags_WidthFixed, 210);
			ImGui::TableSetupColumn("Round by round", ImGuiTableColumnFlags_WidthStretch);
			Headers({{"Fix", nullptr}, {"Rounds", "Of your rounds tonight"}, {"Most often", "Its usual reason"}, {"Typical: you vs them", "Medians, rounds with that reason"},
				{"Round by round", "Filled: it came up"}});
			for (size_t i = 0; i < recur.size() && i < 8; i++)
			{
				const Recur& q = recur[i];
				ImGui::TableNextRow();
				Cell(q.Subject);
				NumCell(std::to_string(q.Rounds) + " of " + std::to_string(n.Played));
				auto most = std::max_element(q.Whats.begin(), q.Whats.end(), [](auto& x1, auto& x2) { return x1.second < x2.second; });
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(most->first.c_str());
				ImGui::SameLine();
				ImGui::TextColored(kMuted, "in %d", most->second);
				// typical: the medians over the rounds with that reason, so the numbers share a unit
				std::vector<double> ys, ts;
				std::string unit;
				for (const auto& e : q.Each) { if (e.What == most->first) { ys.push_back(e.You); ts.push_back(e.Them); unit = e.Unit; } }
				ImGui::TableNextColumn();
				ImGui::TextUnformatted((Num(middle(ys)) + " vs " + Num(middle(ts))).c_str());
				ImGui::SameLine();
				ImGui::TextColored(kMuted, "%s", unit.c_str());
				// a cell per round: filled when it came up, dark when not, outlined when you weren't on this spec; the round
				// picked outlined in white; a click opens that round in You
				ImGui::TableNextColumn();
				ImVec2 p = ImGui::GetCursorScreenPos();
				float avail = ImGui::GetContentRegionAvail().x;
				float cw = std::clamp(avail / std::max(1, count) - 2, 3.0f, 8.0f);
				ImGui::PushID(static_cast<int>(i));
				ImGui::InvisibleButton("cells", ImVec2(std::max(1.0f, (cw + 2) * count), lh));
				bool over = ImGui::IsItemHovered();
				bool clicked = ImGui::IsItemClicked();
				ImGui::PopID();
				ImDrawList* cd = ImGui::GetWindowDrawList();
				for (const NightRound& r : n.Rounds)
				{
					ImVec2 a0(p.x + r.Index * (cw + 2), p.y + 1), a1(a0.x + cw, p.y + lh - 1);
					if (!r.Mine) { cd->AddRect(a0, a1, IM_COL32(0x3b, 0x42, 0x50, 255)); }
					else { cd->AddRectFilled(a0, a1, q.Seen.count(r.Index) ? kPeer : IM_COL32(0x1b, 0x1d, 0x22, 255)); }
					if (r.Index == c.Index) { cd->AddRect(ImVec2(a0.x - 1, a0.y - 1), ImVec2(a1.x + 1, a1.y + 1), ImGui::GetColorU32(ImGuiCol_Text)); }
				}
				if (over)
				{
					int ri = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - p.x) / (cw + 2)), 0, count - 1);
					ImGui::BeginTooltip();
					ImGui::Text("Round %d: %s", ri + 1, !n.Rounds[ri].Mine ? ("not on " + c.You.Spec).c_str() : q.Seen.count(ri) ? "it came up" : "it didn't come up");
					ImGui::TextColored(kMuted, "Latest, round %d: %s", q.Last + 1, q.Latest.c_str());
					ImGui::TextColored(kMuted, "Click to open the round");
					ImGui::EndTooltip();
					if (clicked) { S().Selected = ri == count - 1 ? -1 : ri; S().SwitchTo = T_You; S().AllRounds = false; }
				}
			}
			ImGui::EndTable();
		}
		ImGui::Spacing();
		ImGui::Separator();
		float left = ImGui::GetContentRegionAvail().x * 0.5f;
		float boxH = (lh + ImGui::GetStyle().ItemSpacing.y) * 5 + 6;
		ImGui::BeginChild("yours", ImVec2(left, boxH), false);
		ImGui::TextUnformatted("Your rounds");
		auto line = [&](const char* aLabel, const std::string& aText)
		{
			ImGui::TextColored(kMuted, "%s", aLabel);
			ImGui::SameLine(70);
			ImGui::TextUnformatted(aText.c_str());
		};
		if (best) { line("best", "round " + std::to_string(best->Index + 1) + ", " + RoleText(c, best->Value)); }
		if (worst) { line("worst", "round " + std::to_string(worst->Index + 1) + ", " + RoleText(c, worst->Value)); }
		line("downed", std::to_string(downs) + (downs == 1 ? " time" : " times") + ", died " + std::to_string(deaths));
		line("played", std::to_string(n.Played) + " of " + std::to_string(count) + " rounds on " + c.You.Spec);
		ImGui::EndChild();
		ImGui::SameLine(0, 12);
		ImGui::BeginChild("squadnight", ImVec2(0, boxH), false);
		ImGui::TextUnformatted("The squad tonight");
		int won = 0, lost = 0, squadDowns = 0, inSpikes = 0, windows = 0, covered = 0;
		for (const FightPtr& fp : fights)
		{
			const Fight& f = *fp;
			won += f.EnemyDowns > f.SquadDowns;
			lost += f.EnemyDowns < f.SquadDowns;
			for (int32_t d : f.SquadDownMs)
			{
				squadDowns++;
				bool in = false;
				for (int64_t t : f.TheirSpikesMs) { in |= d >= t - 1000 && d <= t + 4000; }
				inSpikes += in;
			}
			for (auto& [g, w] : f.GroupCcWindows) { windows += w; }
			for (auto& [g, k] : f.GroupCcCovered) { covered += k; }
		}
		line("rounds", std::to_string(won) + " won, " + std::to_string(lost) + " lost, " + std::to_string(count - won - lost) + " even");
		if (squadDowns) { line("downs", std::to_string(inSpikes * 100 / squadDowns) + "% of ours came in an enemy spike"); }
		if (windows) { line("CC", "stability covered " + std::to_string(covered * 100 / windows) + "% of the CC on us"); }
		ImGui::EndChild();
	}

	std::vector<std::string> SummaryLines(const Ctx& c)
	{
		std::vector<std::string> out;
		if (!c.MeRaw) { out.push_back(NoYou(*c.F)); return out; }
		bool known = c.MyRole != R_Heal || c.You.HealKnown;
		double mine = RoleValue(c, c.You);
		int rank = 1, of = 1;
		for (const Player& p : c.Peers)
		{
			if (c.MyRole == R_Heal && !p.HealKnown) { continue; }
			of++;
			rank += RoleValue(c, p) > mine;
		}
		static const char* kOrd[] = {"th", "st", "nd", "rd"};
		std::string line = std::string(RoleLabel(c)) + ": " + (known ? RoleText(c, mine) : std::string("unknown"));
		if (known && of > 1)
		{
			line += ", " + std::to_string(rank) + kOrd[(rank % 10 < 4 && (rank / 10) % 10 != 1) ? rank % 10 : 0] + " of " + std::to_string(of) + " " + c.PeerLabel() + "s";
		}
		out.push_back(line);
		std::vector<Fix> fixes = FindFixes(c);
		if (!fixes.empty())
		{
			const Fix& f = fixes[0];
			out.push_back("Fix first: " + f.Subject + ", " + f.What + " (" + f.YouText + " vs " + f.ThemText + " " + f.Unit + ")");
		}
		else if (c.Vs) { out.push_back("Nothing stands out against " + c.Vs->Name + "."); }
		return out;
	}
}
