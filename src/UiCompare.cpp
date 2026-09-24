// Compare tab: every skill of one metric, you against the compared player: output, gap, casts, when you each
// cast it and why. A row opens the skill detail in place. Review's measure level shows the same table.
#include <algorithm>

#include "imgui/imgui.h"

#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		constexpr int kColumns = 8;

		bool BeginSkillTable(const char* aId, int aMetric, bool aLanes, bool aHeaders, const std::string& aVsName)
		{
			ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
			if (!ImGui::BeginTable(aId, kColumns, flags)) { return false; }
			const Metric& m = Metrics()[aMetric];
			static std::string you, vs, window, windowTip;
			static const char* kShort[] = {"In our spike", "Before enemy spike", "After enemy spike"};
			std::string unit = m.What == K_Boon ? "" : std::string(" ") + RateUnit(m); // boons: the unit is in the metric's name
			you = "You" + unit;
			vs = aVsName + unit;
			window = kShort[TimingWindow(aMetric)];
			windowTip = std::string("Casts ") + WindowLabel(TimingWindow(aMetric)) + ": yours vs theirs";
			ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthFixed, 175);
			ImGui::TableSetupColumn(you.c_str(), ImGuiTableColumnFlags_WidthFixed, 90);
			ImGui::TableSetupColumn(vs.c_str(), ImGuiTableColumnFlags_WidthFixed, 90);
			ImGui::TableSetupColumn("Gap", ImGuiTableColumnFlags_WidthFixed, 60);
			ImGui::TableSetupColumn("Casts", ImGuiTableColumnFlags_WidthFixed, 60);
			ImGui::TableSetupColumn("When cast", ImGuiTableColumnFlags_WidthFixed, aLanes ? 150.0f : 60.0f);
			ImGui::TableSetupColumn(window.c_str(), ImGuiTableColumnFlags_WidthFixed, 135);
			ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthFixed, 115);
			if (aHeaders)
			{
				Headers({{"Skill", "Click for the detail"}, {you.c_str(), "Your output from it"}, {vs.c_str(), "Their output from it"},
					{"Gap", "Theirs minus yours"}, {"Casts", "Yours vs theirs"}, {"When cast", "You above, them below"},
					{window.c_str(), windowTip.c_str()}, {"Why", "The main reason"}});
			}
			return true;
		}
	}

	void SkillTable(const Ctx& c, int aMetric, bool aInReview)
	{
		if (!c.MeRaw) { ImGui::TextColored(kMuted, "This log's recorder isn't in the squad."); return; }
		if (!c.Vs) { ImGui::TextColored(kMuted, "Nobody to compare with in this round."); return; }
		State& s = S();
		const Metric& m = Metrics()[aMetric];
		const Player& you = c.You;
		const Player& vs = *c.Vs;
		if (!Known(m, you) || !Known(m, vs)) { ImGui::TextColored(kMuted, "Healing is unknown for one of you: it needs Healing Stats on that side."); return; }

		auto val = [&](const Player& p, int32_t aSkill) { const SkillRow* r = Row(p, aSkill); return r ? Rate(m, p, m.PerSkill(*r)) : 0.0; };
		auto casts = [&](const Player& p, int32_t aSkill) { const SkillRow* r = Row(p, aSkill); return r ? r->Casts : 0; };
		std::vector<int32_t> producing, rest;
		for (const Player* p : {&you, &vs})
		{
			for (auto& [sk, r] : p->Skills)
			{
				if (std::find(producing.begin(), producing.end(), sk) != producing.end() || std::find(rest.begin(), rest.end(), sk) != rest.end()) { continue; }
				(val(you, sk) > 0 || val(vs, sk) > 0 ? producing : rest).push_back(sk);
			}
		}
		std::sort(producing.begin(), producing.end(), [&](int32_t a, int32_t b)
		{
			double ga = val(vs, a) - val(you, a), gb = val(vs, b) - val(you, b);
			return ga != gb ? ga > gb : val(vs, a) > val(vs, b);
		});
		std::sort(rest.begin(), rest.end(), [&](int32_t a, int32_t b) { return casts(you, a) + casts(vs, a) > casts(you, b) + casts(vs, b); });

		// The answer
		double ry = Rate(m, you, m.Total(you)), rt = Rate(m, vs, m.Total(vs));
		std::vector<std::string> top;
		for (int32_t sk : producing)
		{
			if (val(vs, sk) > val(you, sk) && top.size() < 2) { top.push_back(sk == 0 ? "other sources (traits, relics, runes)" : c.Name(sk)); }
		}
		std::string unit = RateUnit(m);
		if (rt > ry)
		{
			std::string from = top.empty() ? "" : top.size() == 1 ? ", mostly from " + top[0] : ", mostly from " + top[0] + " and " + top[1];
			Answer(vs.Name + " got " + Num(rt - ry) + " " + unit + " more " + Lower(m.Name) + from + ": " + Num(rt) + " against your " + Num(ry) + ".");
		}
		else { Answer("You got more " + Lower(m.Name) + " than " + vs.Name + ": " + Num(ry) + " " + unit + " against " + Num(rt) + "."); }
		Key(kYou, "you");
		Key(kPeer, vs.Name.c_str());
		if (c.OneRound)
		{
			int k = TimingWindow(aMetric);
			Key(kOurBand, "our spike");
			Key(k == Analysis::T_AheadOfTheirs ? kEnemyPre : kEnemyBand, k == Analysis::T_AheadOfTheirs ? "4 s before an enemy spike" : "enemy spike and 3 s after");
		}
		ImGui::NewLine();

		double maxRate = 0;
		for (int32_t sk : producing) { maxRate = std::max({maxRate, val(you, sk), val(vs, sk)}); }
		const int k = TimingWindow(aMetric);
		const bool lanes = c.OneRound && c.MeRaw;
		int chunk = 0;
		std::string id = "skills0";
		bool open = BeginSkillTable(id.c_str(), aMetric, lanes, true, vs.Name);
		for (int32_t sk : producing)
		{
			if (!open) { break; }
			Why w = Explain(you, vs, vs.Name, aMetric, sk, c.OneRound);
			const SkillRow* ry2 = Row(you, sk);
			const SkillRow* rt2 = Row(vs, sk);
			float lh = ImGui::GetTextLineHeight();
			ImGui::TableNextRow(0, lanes ? lh + 6 : 0);
			ImGui::TableNextColumn();
			bool expanded = !aInReview && s.CompareOpen == sk;
			ImGui::PushID(sk);
			if (ImGui::Selectable("##row", expanded, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap, ImVec2(0, lanes ? lh + 4 : 0)))
			{
				if (aInReview) { s.Skill = sk; }
				else { s.CompareOpen = expanded ? kNoSkill : sk; expanded = !expanded; }
			}
			ImGui::PopID();
			ImGui::SameLine(0, 0);
			SkillIcon(sk, c.Name(sk));
			ImGui::TextUnformatted(c.Name(sk).c_str());
			BarCell(val(you, sk), maxRate, kYou, Num(val(you, sk)));
			BarCell(val(vs, sk), maxRate, kPeer, Num(val(vs, sk)));
			double gap = val(you, sk) - val(vs, sk);
			NumCell((gap > 0 ? "+" : "") + Num(gap));
			int cy = casts(you, sk), ct = casts(vs, sk);
			NumCell(cy || ct ? std::to_string(cy) + " vs " + std::to_string(ct) : "-", cy || ct ? nullptr : &kMuted);
			ImGui::TableNextColumn();
			if (lanes && (cy || ct))
			{
				static const std::vector<int32_t> kNone;
				const SkillRow* a = Row(*c.MeRaw, sk);
				const SkillRow* b = c.VsRaw ? Row(*c.VsRaw, sk) : nullptr;
				float w2 = ImGui::GetContentRegionAvail().x;
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
				Lane(*c.F, a ? a->CastMs : kNone, kYou, w2, lh * 0.5f, k);
				Lane(*c.F, b ? b->CastMs : kNone, kPeerTick, w2, lh * 0.5f, k);
				ImGui::PopStyleVar();
			}
			else { ImGui::TextColored(kMuted, cy || ct ? "" : "no casts"); }
			if (cy || ct)
			{
				NumCell(std::to_string(ry2 ? ry2->Timing[k] : 0) + " of " + std::to_string(cy) + " vs " + std::to_string(rt2 ? rt2->Timing[k] : 0) + " of " + std::to_string(ct));
			}
			else { Cell(""); }
			Cell(w.Word);
			if (expanded)
			{
				ImGui::EndTable();
				ImGui::Indent(24);
				ImGui::Spacing();
				SkillDetail(c, aMetric, sk);
				ImGui::Spacing();
				ImGui::Unindent(24);
				id = "skills" + std::to_string(++chunk);
				open = BeginSkillTable(id.c_str(), aMetric, lanes, false, vs.Name);
			}
		}
		if (open) { ImGui::EndTable(); }
		if (aMetric == kMetricHeal)
		{
			ImGui::TextColored(kMuted, "Healing on downed allies: you %s, %s %s. Healing Stats counts it; TopStats leaves it out.",
				Num(double(you.HealDowned)).c_str(), vs.Name.c_str(), Num(double(vs.HealDowned)).c_str());
		}
		if (m.What == K_Count) { ImGui::TextColored(kMuted, "Cleanses and strips count for a skill only if the GW2 API says it removes them."); }

		// Casts of skills that gave none of this: only useful for damage and healing, and only in Compare
		if (!aInReview && m.What != K_Boon && !rest.empty() && ImGui::CollapsingHeader(("Skills that added no " + Lower(m.Name)).c_str()))
		{
			if (ImGui::BeginTable("rest", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthFixed, 210);
				ImGui::TableSetupColumn("You /min", ImGuiTableColumnFlags_WidthFixed, 80);
				ImGui::TableSetupColumn("Them /min", ImGuiTableColumnFlags_WidthFixed, 80);
				Headers({{"Skill", nullptr}, {"You /min", "Your casts per minute"}, {"Them /min", "Their casts per minute"}});
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

	void CompareTab(const Ctx& c)
	{
		if (!c.MeRaw) { ImGui::TextColored(kMuted, "This log's recorder isn't in the squad."); return; }
		State& s = S();
		const auto& metrics = Metrics();
		int metric = s.Metric >= 0 ? std::clamp(s.Metric, 0, static_cast<int>(metrics.size()) - 1) : c.RoleMetric;
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
	}
}
