// Compare tab (why did A beat B): the measure, down contribution and spike timing side by side, then every skill's
// gap as a bar from a middle line, grouped (only one of them used it, both did), the biggest first and the rest
// folded. A row opens the skill detail in place. The You tab's measure level shows the same list.
#include <algorithm>
#include <cmath>
#include <set>

#include "imgui/imgui.h"

#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		// One skill in the gap list: a bar from the middle line (left: the left player got more, right: the right player)
		struct GapRow { int32_t Skill; double Left, Right; int CastsL, CastsR; };

		// The top of Compare: the measure itself, and for damage down contribution and damage in our spikes; for
		// healing and barrier, the share landing in enemy spikes
		void Stats(const Ctx& c, int aMetric)
		{
			const Metric& m = Metrics()[aMetric];
			const Player& a = c.You;
			const Player& b = *c.Vs;
			struct Line { std::string Label, A, B, Note; };
			std::vector<Line> lines;
			double ra = Rate(m, a, m.Total(a)), rb = Rate(m, b, m.Total(b));
			std::string unit = m.What == K_Boon ? "" : std::string(" ") + RateUnit(m);
			std::string lead = ra > rb * 1.02 ? c.LeftName() + " +" + Num(ra - rb) + unit : rb > ra * 1.02 ? b.Name + " +" + Num(rb - ra) + unit : "about the same";
			lines.push_back({m.Name + unit, Known(m, a) ? Num(ra) : "unknown", Known(m, b) ? Num(rb) : "unknown", lead});
			if (aMetric == kMetricDamage || aMetric == kMetricDamageAll)
			{
				auto dc = [](const Player& p) { return Num(double(p.DownContribution)) + (p.Damage > 0 ? " (" + std::to_string(int(100.0 * p.DownContribution / p.Damage + 0.5)) + "%)" : ""); };
				lines.push_back({"Down contribution", dc(a), dc(b), "estimated: damage from 90% to a down that died, and its share"});
				double sa = SpikeShare(Lookup(c, a).first, Lookup(c, a).second, a.Spec), sb = SpikeShare(Lookup(c, b).first, Lookup(c, b).second, b.Spec);
				auto pct = [](double v) { return v < 0 ? std::string("-") : std::to_string(int(v + 0.5)) + "%"; };
				lines.push_back({"Damage in our spikes", pct(sa), pct(sb), "share of their damage within 2 s of our spike peaks"});
			}
			else if (aMetric == kMetricHeal || aMetric == kMetricBarrier)
			{
				double sa = HealInEnemySpikes(Lookup(c, a).first, Lookup(c, a).second, a.Spec), sb = HealInEnemySpikes(Lookup(c, b).first, Lookup(c, b).second, b.Spec);
				auto pct = [](double v) { return v < 0 ? std::string("unknown") : std::to_string(int(v + 0.5)) + "%"; };
				lines.push_back({"Healing in enemy spikes", pct(sa), pct(sb), "share of their healing in an enemy spike or the 3 s after"});
			}
			if (ImGui::BeginTable("stats", 4, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 190);
				ImGui::TableSetupColumn(c.LeftName().c_str(), ImGuiTableColumnFlags_WidthFixed, 120);
				ImGui::TableSetupColumn(b.Name.c_str(), ImGuiTableColumnFlags_WidthFixed, 120);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();
				Cell("");
				NumCell(c.LeftName(), &kMuted);
				NumCell(b.Name, &kMuted);
				Cell("");
				for (const Line& l : lines)
				{
					ImGui::TableNextRow();
					Cell(l.Label);
					NumCell(l.A);
					NumCell(l.B);
					Cell(l.Note, &kMuted);
				}
				ImGui::EndTable();
			}
		}
	}

	void SkillTable(const Ctx& c, int aMetric, bool aInReview)
	{
		if (!c.MeRaw) { ImGui::TextColored(kMuted, "%s", NoYou(*c.F).c_str()); return; }
		if (!c.Vs) { ImGui::TextColored(kMuted, "Nobody to compare with in this round."); return; }
		State& s = S();
		const Metric& m = Metrics()[aMetric];
		const Player& you = c.You;
		const Player& vs = *c.Vs;
		if (!Known(m, you) || !Known(m, vs)) { ImGui::TextColored(kMuted, "Healing is unknown for one of them: it needs Healing Stats on that side."); return; }

		auto val = [&](const Player& p, int32_t aSkill) { const SkillRow* r = Row(p, aSkill); return r ? Rate(m, p, m.PerSkill(*r)) : 0.0; };
		auto casts = [&](const Player& p, int32_t aSkill) { const SkillRow* r = Row(p, aSkill); return r ? r->Casts : 0; };
		std::vector<GapRow> only, both;
		std::vector<int32_t> rest;
		std::set<int32_t> seen;
		for (const Player* p : {&you, &vs})
		{
			for (auto& [sk, r] : p->Skills)
			{
				if (!seen.insert(sk).second) { continue; }
				GapRow g{sk, val(you, sk), val(vs, sk), casts(you, sk), casts(vs, sk)};
				if (g.Left <= 0 && g.Right <= 0) { rest.push_back(sk); continue; }
				// "only one used it": one of them got nothing from it
				(g.Left <= 0 || g.Right <= 0 ? only : both).push_back(g);
			}
		}
		auto bigger = [](const GapRow& a, const GapRow& b) { return std::fabs(a.Right - a.Left) > std::fabs(b.Right - b.Left); };
		std::sort(only.begin(), only.end(), bigger);
		std::sort(both.begin(), both.end(), bigger);
		std::sort(rest.begin(), rest.end(), [&](int32_t a, int32_t b) { return casts(you, a) + casts(vs, a) > casts(you, b) + casts(vs, b); });

		// The answer
		double ry = Rate(m, you, m.Total(you)), rt = Rate(m, vs, m.Total(vs));
		std::vector<GapRow> all = only;
		all.insert(all.end(), both.begin(), both.end());
		std::sort(all.begin(), all.end(), [](const GapRow& a, const GapRow& b) { return a.Right - a.Left > b.Right - b.Left; });
		std::string unit = RateUnit(m);
		std::vector<std::string> top;
		for (const GapRow& g : all) { if (g.Right > g.Left && top.size() < 2) { top.push_back(g.Skill == 0 ? "other sources (traits, relics, runes)" : c.Name(g.Skill)); } }
		if (rt > ry)
		{
			std::string from = top.empty() ? "" : top.size() == 1 ? ", mostly from " + top[0] : ", mostly from " + top[0] + " and " + top[1];
			std::string yours = c.LeftIsYou() ? "your " : c.You.Name + "'s ";
			Answer(vs.Name + " got " + Num(rt - ry) + " " + unit + " more " + Lower(m.Name) + from + ": " + Num(rt) + " against " + yours + Num(ry) + ".");
		}
		else if (ry <= 0 && rt <= 0) { Answer("Neither " + (c.LeftIsYou() ? std::string("you") : c.You.Name) + " nor " + vs.Name + " got any " + Lower(m.Name) + "."); }
		else if (ry <= rt * 1.02) { Answer(c.LeftName() + " and " + vs.Name + " got about the same " + Lower(m.Name) + ": " + Num(ry) + " " + unit + " against " + Num(rt) + "."); }
		else { Answer(c.LeftName() + " got more " + Lower(m.Name) + " than " + vs.Name + ": " + Num(ry) + " " + unit + " against " + Num(rt) + "."); }
		if (!aInReview) { Stats(c, aMetric); }

		// The key
		ImGui::Spacing();
		const std::string leftShort = c.LeftIsYou() ? "you" : c.You.Name;
		Key(kYou, (leftShort + " got more").c_str());
		Key(kPeerTick, (vs.Name + " got more").c_str());
		ImGui::TextColored(kMuted, "%s from each skill, bars from the middle line", (Lower(m.Name) + (m.What == K_Boon ? "" : " " + unit)).c_str());
		const int k = TimingWindow(aMetric);
		const char* timingHead = k == Analysis::T_IntoOurs ? "In our spike" : k == Analysis::T_AheadOfTheirs ? "Before enemy spike" : "In an enemy spike";

		// The rows: a group for skills only one of them got anything from, one for the rest; each shows its biggest
		// gaps and folds the rest into one line
		float lh = ImGui::GetTextLineHeight();
		const float nameW = 210, barW = 360, timeX = nameW + barW + 16, whyX = timeX + 120;
		double maxGap = 1e-9;
		for (const GapRow& g : all) { maxGap = std::max(maxGap, std::fabs(g.Right - g.Left)); }
		{
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImU32 muted = ImGui::GetColorU32(kMuted);
			dl->AddText(p, muted, "Skill");
			std::string l = "< " + leftShort + " more", r = vs.Name + " more >";
			dl->AddText(ImVec2(p.x + nameW, p.y), muted, l.c_str());
			dl->AddText(ImVec2(p.x + nameW + barW - ImGui::CalcTextSize(r.c_str()).x, p.y), muted, r.c_str());
			dl->AddText(ImVec2(p.x + timeX, p.y), muted, timingHead);
			dl->AddText(ImVec2(p.x + whyX, p.y), muted, "Why");
			ImGui::Dummy(ImVec2(1, lh));
			if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Casts %s: left, then right", WindowLabel(k)); }
		}
		auto row = [&](const GapRow& g)
		{
			ImGui::PushID(g.Skill);
			ImVec2 p = ImGui::GetCursorScreenPos();
			bool expanded = !aInReview && s.CompareOpen == g.Skill;
			if (ImGui::Selectable("##gap", expanded, 0, ImVec2(0, lh + 4)))
			{
				if (aInReview) { s.Skill = g.Skill; }
				else { s.CompareOpen = expanded ? kNoSkill : g.Skill; expanded = !expanded; }
			}
			ImGui::PopID();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			std::string name = g.Skill == 0 ? "Other sources" : c.Name(g.Skill);
			IconAt(dl, ImVec2(p.x, p.y + 2), lh, g.Skill, name);
			dl->PushClipRect(p, ImVec2(p.x + nameW - 6, p.y + lh + 4), true);
			dl->AddText(ImVec2(p.x + lh + 5, p.y + 2), ink, name.c_str());
			dl->PopClipRect();
			// the bar from the middle
			float mid = p.x + nameW + barW * 0.5f;
			dl->AddLine(ImVec2(mid, p.y), ImVec2(mid, p.y + lh + 4), IM_COL32(0x5a, 0x5f, 0x68, 255));
			double gap = g.Right - g.Left;
			float w = std::max(2.0f, static_cast<float>(std::fabs(gap) / maxGap) * (barW * 0.5f - 50));
			std::string v = (gap > 0 ? "+" : "") + Num(gap);
			if (gap < 0)
			{
				dl->AddRectFilled(ImVec2(mid - w, p.y + 6), ImVec2(mid, p.y + lh - 2), kYou);
				dl->AddText(ImVec2(mid - w - 4 - ImGui::CalcTextSize(v.c_str()).x, p.y + 2), ink, v.c_str());
			}
			else
			{
				dl->AddRectFilled(ImVec2(mid + 1, p.y + 6), ImVec2(mid + 1 + w, p.y + lh - 2), kPeerTick);
				dl->AddText(ImVec2(mid + w + 5, p.y + 2), ink, v.c_str());
			}
			// timing: casts in the window, left then right
			const SkillRow* a = Row(you, g.Skill);
			const SkillRow* b = Row(vs, g.Skill);
			auto part = [&](const SkillRow* r, int n) { return n ? std::to_string(r ? r->Timing[k] : 0) + " of " + std::to_string(n) : std::string("-"); };
			std::string timing = g.CastsL || g.CastsR ? part(a, g.CastsL) + " \xc2\xb7 " + part(b, g.CastsR) : std::string("no casts");
			dl->AddText(ImVec2(p.x + timeX, p.y + 2), g.CastsL || g.CastsR ? ink : muted, timing.c_str());
			// why: the reason on the side that got less
			std::string why;
			if (g.Left <= 0) { why = "only " + vs.Name + (g.CastsR ? " (" + std::to_string(g.CastsR) + (g.CastsR == 1 ? " cast)" : " casts)") : ""); }
			else if (g.Right <= 0) { why = "only " + leftShort + (g.CastsL ? " (" + std::to_string(g.CastsL) + (g.CastsL == 1 ? " cast)" : " casts)") : ""); }
			else if (gap > 0) { why = leftShort + ": " + Explain(you, vs, vs.Name, aMetric, g.Skill, c.OneRound, c.LeftIsYou() ? "" : you.Name).Word; }
			else { why = vs.Name + ": " + Explain(vs, you, leftShort, aMetric, g.Skill, c.OneRound, vs.Name).Word; }
			dl->AddText(ImVec2(p.x + whyX, p.y + 2), muted, why.c_str());
			if (expanded)
			{
				ImGui::Indent(24);
				ImGui::Spacing();
				SkillDetail(c, aMetric, g.Skill, true);
				ImGui::Spacing();
				ImGui::Unindent(24);
			}
		};
		auto group = [&](const char* aTitle, const std::vector<GapRow>& aRows, bool& aMore)
		{
			if (aRows.empty()) { return; }
			double l = 0, r = 0;
			for (const GapRow& g : aRows) { if (g.Right > g.Left) { r += g.Right - g.Left; } else { l += g.Left - g.Right; } }
			ImGui::Spacing();
			ImGui::TextUnformatted(aTitle);
			ImGui::SameLine();
			ImGui::TextColored(kMuted, "%s +%s, %s +%s %s", leftShort.c_str(), Num(l).c_str(), vs.Name.c_str(), Num(r).c_str(), unit.c_str());
			const size_t kShown = 5;
			size_t n = aMore ? aRows.size() : std::min(kShown, aRows.size());
			for (size_t i = 0; i < n; i++) { row(aRows[i]); }
			if (aRows.size() > kShown)
			{
				double sum = 0;
				for (size_t i = kShown; i < aRows.size(); i++) { sum += std::fabs(aRows[i].Right - aRows[i].Left); }
				std::string label = aMore ? std::string("Show the biggest only") : "+ " + std::to_string(aRows.size() - kShown) + " smaller (together " + Num(sum) + " " + unit + ")";
				ImGui::PushID(aTitle);
				if (ImGui::SmallButton(label.c_str())) { aMore = !aMore; }
				ImGui::PopID();
			}
		};
		group("Only one of them used it", only, s.CompareMore[0]);
		group("Both used it", both, s.CompareMore[1]);
		if (aMetric == kMetricHeal)
		{
			ImGui::TextColored(kMuted, "Healing on downed allies: %s %s, %s %s. Healing Stats counts it; TopStats leaves it out.",
				c.LeftIsYou() ? "you" : you.Name.c_str(), Num(double(you.HealDowned)).c_str(), vs.Name.c_str(), Num(double(vs.HealDowned)).c_str());
		}
		if (m.What == K_Count) { ImGui::TextColored(kMuted, "Cleanses and strips count for a skill only if the GW2 API says it removes them."); }

		// Casts of skills that gave none of this: only useful for damage and healing, and only in Compare
		if (!aInReview && m.What != K_Boon && !rest.empty() && ImGui::CollapsingHeader(("Skills that added no " + Lower(m.Name)).c_str()))
		{
			if (ImGui::BeginTable("rest", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthFixed, 210);
				const std::string leftHead = c.LeftName() + " /min", rightHead = vs.Name + " /min";
				ImGui::TableSetupColumn(leftHead.c_str(), ImGuiTableColumnFlags_WidthFixed, 110);
				ImGui::TableSetupColumn(rightHead.c_str(), ImGuiTableColumnFlags_WidthFixed, 110);
				Headers({{"Skill", nullptr}, {leftHead.c_str(), "Casts per minute"}, {rightHead.c_str(), "Casts per minute"}});
				for (int32_t sk : rest)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					SkillIcon(sk, c.Name(sk));
					ImGui::TextUnformatted(c.Name(sk).c_str());
					NumCell(Num(PerS(you, casts(you, sk)) * 60));
					NumCell(Num(PerS(vs, casts(vs, sk)) * 60), &kMuted);
				}
				ImGui::EndTable();
			}
		}
	}

	namespace
	{
		// The two players over the round, second by second: the left player up, the right one down, on the spike bands.
		// For the measures the log has in time: damage, healing, cleanses, strips.
		void OverTime(const Ctx& c, int aMetric)
		{
			const Fight& f = *c.F;
			if (!c.VsRaw) { return; }
			const Player& a = *c.MeRaw;
			const Player& b = *c.VsRaw;
			auto series = [&](const Player& p) -> std::vector<double>
			{
				size_t n = static_cast<size_t>(f.DurationMs / 1000 + 1);
				std::vector<double> v(n, 0.0);
				auto count = [&](const std::vector<int32_t>& ms) { for (int32_t t : ms) { if (size_t s = static_cast<size_t>(t / 1000); s < n) { v[s] += 1; } } };
				if (aMetric == kMetricDamage || aMetric == kMetricDamageAll) { for (size_t s = 0; s < n && s < p.DamagePerS.size(); s++) { v[s] = p.DamagePerS[s]; } }
				else if (aMetric == kMetricHeal) { for (size_t s = 0; s < n && s < p.HealPerS.size(); s++) { v[s] = p.HealPerS[s]; } }
				else if (aMetric == kMetricCleanses) { count(p.CleanseMs); }
				else if (aMetric == kMetricStrips) { count(p.StripMs); }
				else { v.clear(); }
				return v;
			};
			std::vector<double> up = series(a), down = series(b);
			ImGui::Spacing();
			ImGui::Separator();
			const Metric& m = Metrics()[aMetric];
			if (up.empty())
			{
				ImGui::TextColored(kMuted, "Over time: not available for %s yet (damage, healing, cleanses and strips are).", Lower(m.Name).c_str());
				return;
			}
			if (!c.OneRound) { ImGui::TextColored(kMuted, "Over time shows the round picked at the top."); }
			std::string what = aMetric == kMetricCleanses || aMetric == kMetricStrips ? Lower(m.Name) + " per second" : Lower(m.Name) + " /s";
			ImGui::Text("%s and %s over the round, %s", c.LeftName().c_str(), b.Name.c_str(), what.c_str());
			ImGui::SameLine(0, 20);
			Key(kYou, c.LeftIsYou() ? "you" : a.Name.c_str());
			Key(kPeerTick, b.Name.c_str());
			Key(kOurBand, "our spike");
			Key(kEnemyBand, "enemy spike");
			ImGui::NewLine();

			const float labelW = 200, lh = ImGui::GetTextLineHeight();
			float width = std::max(300.0f, ImGui::GetContentRegionAvail().x - labelW - 8), h = lh * 8;
			double mx = 1;
			for (double v : up) { mx = std::max(mx, v); }
			for (double v : down) { mx = std::max(mx, v); }
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 top = ImGui::GetCursorScreenPos();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
			dl->AddText(top, ink, c.LeftIsYou() ? "You" : a.Name.c_str());
			dl->AddText(ImVec2(top.x, top.y + h - lh), ink, b.Name.c_str());
			SmallText(dl, ImVec2(top.x, top.y + h * 0.5f - lh * 0.5f), muted, "peak " + Num(mx));
			ImGui::SetCursorScreenPos(ImVec2(top.x + labelW, top.y));
			ImVec2 g = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("overtime", ImVec2(width, h));
			bool hovered = ImGui::IsItemHovered();
			double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
			auto x = [&](double ms) { return g.x + static_cast<float>(std::clamp(ms / span, 0.0, 1.0)) * width; };
			float mid = g.y + h * 0.5f;
			dl->AddRectFilled(g, ImVec2(g.x + width, g.y + h), kLaneBg);
			for (int64_t t : f.OurSpikesMs) { dl->AddRectFilled(ImVec2(x(t - 2000.0), g.y), ImVec2(x(t + 2000.0), g.y + h), kOurBand); }
			for (int64_t t : f.TheirSpikesMs) { dl->AddRectFilled(ImVec2(x(double(t)), g.y), ImVec2(x(t + 3000.0), g.y + h), kEnemyBand); }
			float bw = std::max(1.0f, width / std::max<size_t>(1, up.size()) * 0.72f);
			for (size_t s = 0; s < up.size(); s++)
			{
				float bx = x(s * 1000.0);
				float hu = static_cast<float>(up[s] / mx) * h * 0.5f, hd = s < down.size() ? static_cast<float>(down[s] / mx) * h * 0.5f : 0.0f;
				if (hu > 0) { Rect(dl, ImVec2(bx, mid - hu), bw, hu, kYou); }
				if (hd > 0) { Rect(dl, ImVec2(bx, mid), bw, hd, kPeerTick); }
			}
			dl->AddLine(ImVec2(g.x, mid), ImVec2(g.x + width, mid), IM_COL32(58, 62, 69, 255));
			if (hovered)
			{
				int sec = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - g.x) / width * span / 1000.0), 0, static_cast<int>(up.size()) - 1);
				float cx = x(sec * 1000.0 + 500.0);
				dl->AddLine(ImVec2(cx, g.y), ImVec2(cx, g.y + h), ink);
				ImGui::SetTooltip("%s\n%s: %s\n%s: %s", Duration(sec * 1000LL).c_str(), c.LeftName().c_str(), Num(up[sec]).c_str(), b.Name.c_str(), Num(sec < static_cast<int>(down.size()) ? down[sec] : 0.0).c_str());
			}
			TimeAxis(f, g.x, width);
		}
	}

	void CompareTab(const Ctx& c)
	{
		if (!c.MeRaw) { ImGui::TextColored(kMuted, "%s", NoYou(*c.F).c_str()); return; }
		State& s = S();
		const auto& metrics = Metrics();
		int metric = s.Metric >= 0 ? std::clamp(s.Metric, 0, static_cast<int>(metrics.size()) - 1) : c.RoleMetric;
		// Any two players of this round: you and your compared player unless picked here
		// A player picker: the class icon, then the name and the spec, in the box and in the list
		auto pick = [&](const char* aId, std::string& aAccount, const std::string& aShown, const Player* aShownPlayer)
		{
			if (aShownPlayer) { SpecIcon(*aShownPlayer); }
			ImGui::SetNextItemWidth(220);
			if (ImGui::BeginCombo(aId, aShown.c_str()))
			{
				std::vector<const Player*> list;
				for (const Player& p : c.F->Players) { list.push_back(&p); }
				std::sort(list.begin(), list.end(), [](const Player* a, const Player* b) { return a->Spec != b->Spec ? a->Spec < b->Spec : a->Name < b->Name; });
				if (ImGui::Selectable("(default)", aAccount.empty())) { aAccount.clear(); s.CompareOpen = kNoSkill; }
				for (const Player* p : list)
				{
					SpecIcon(*p);
					std::string item = p->Name + (p->Pov ? " (you)" : "") + "  " + p->Spec + "##" + p->Account;
					if (ImGui::Selectable(item.c_str(), aAccount == p->Account)) { aAccount = p->Account; s.CompareOpen = kNoSkill; }
				}
				ImGui::EndCombo();
			}
		};
		ImGui::TextUnformatted("Compare");
		ImGui::SameLine();
		pick("##left", s.CompareLeft, c.You.Name + (c.LeftIsYou() ? " (you)" : "") + "  " + c.You.Spec, &c.You);
		ImGui::SameLine();
		ImGui::TextUnformatted("against");
		ImGui::SameLine();
		pick("##right", s.CompareRight, c.Vs ? c.Vs->Name + "  " + c.Vs->Spec : std::string("nobody"), c.Vs);
		ImGui::SameLine(0, 20);
		ImGui::TextColored(kMuted, "in");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(220);
		if (ImGui::BeginCombo("##metric", metrics[metric].Name.c_str()))
		{
			for (int i = 0; i < static_cast<int>(metrics.size()); i++)
			{
				if (i == kMetricGroupBoon || i == kMetricGroupBoon + Analysis::kBoons) { ImGui::Separator(); }
				if (ImGui::Selectable(metrics[i].Name.c_str(), i == metric)) { s.Metric = i; s.CompareOpen = kNoSkill; }
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Follows your role; pick any other"); }
		SkillTable(c, metric, false);
		OverTime(c, metric);
	}
}
