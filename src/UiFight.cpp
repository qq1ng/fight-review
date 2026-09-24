// Fight tab (the selected round): both sides' damage per second on one axis (ours up, the enemy's down), spike
// bands, downs, a readout for the second under the mouse; under it your casts, the compared player's, stability
// on you, and when you were crowd controlled or down. A table shows the same by spike.
#include <algorithm>
#include <cmath>

#include "imgui/imgui.h"

#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		constexpr float kLabelW = 200;

		double NiceMax(double v)
		{
			if (v <= 0) { return 1000; }
			double step = std::pow(10.0, std::floor(std::log10(v)));
			for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) { if (m * step >= v) { return m * step; } }
			return 10 * step;
		}

		int DownsNear(const std::vector<int32_t>& aDowns, int64_t aFrom, int64_t aTo)
		{
			int n = 0;
			for (int32_t d : aDowns) { n += d >= aFrom && d <= aTo; }
			return n;
		}

		// Casts of the skills that produced this metric (for damage players: damage skills)
		std::vector<int32_t> CastTimes(const Player& p, int aMetric)
		{
			const Metric& m = Metrics()[aMetric];
			std::vector<int32_t> out;
			for (auto& [sk, r] : p.Skills) { if (m.PerSkill(r) > 0) { out.insert(out.end(), r.CastMs.begin(), r.CastMs.end()); } }
			return out;
		}

		// Downs as small triangles with a count, grouped within 2 s
		void DownMarks(const Fight& f, const std::vector<int32_t>& aDowns, float aX, float aWidth, bool aOurs)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 p = ImGui::GetCursorScreenPos();
			float lh = ImGui::GetTextLineHeight(), s = lh * 0.5f;
			std::vector<int32_t> d = aDowns;
			std::sort(d.begin(), d.end());
			ImU32 col = aOurs ? kEnemy : kYou, ink = ImGui::GetColorU32(ImGuiCol_Text);
			for (size_t i = 0; i < d.size();)
			{
				size_t j = i;
				while (j < d.size() && d[j] - d[i] <= 2000) { j++; }
				float x = aX + static_cast<float>(double(d[i]) / std::max<int64_t>(1, f.DurationMs)) * aWidth;
				float y = p.y + lh * 0.5f;
				if (aOurs) { dl->AddTriangleFilled(ImVec2(x - s * 0.5f, y - s * 0.5f), ImVec2(x + s * 0.5f, y - s * 0.5f), ImVec2(x, y + s * 0.5f), col); }
				else { dl->AddTriangleFilled(ImVec2(x - s * 0.5f, y + s * 0.5f), ImVec2(x + s * 0.5f, y + s * 0.5f), ImVec2(x, y - s * 0.5f), col); }
				SmallText(dl, ImVec2(x + s * 0.6f, p.y + 1), ink, std::to_string(j - i));
				i = j;
			}
			ImGui::Dummy(ImVec2(aWidth, lh));
		}

		void Spans(const Fight& f, const std::vector<std::pair<int32_t, int32_t>>& aSpans, ImU32 aColor, float aWidth, float aHeight)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 p = ImGui::GetCursorScreenPos();
			double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
			dl->AddRectFilled(p, ImVec2(p.x + aWidth, p.y + aHeight), kLaneBg);
			for (auto& [a, b] : aSpans)
			{
				dl->AddRectFilled(ImVec2(p.x + static_cast<float>(a / span) * aWidth, p.y + 1), ImVec2(p.x + static_cast<float>(b / span) * aWidth, p.y + aHeight - 1), aColor);
			}
			ImGui::Dummy(ImVec2(aWidth, aHeight));
		}

		void Track(const char* aLabel)
		{
			ImGui::TextColored(kMuted, "%s", aLabel);
			ImGui::SameLine(kLabelW);
		}

		void SpikeTable(const Fight& f)
		{
			if (!ImGui::BeginTable("spikes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) { return; }
			ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 60);
			ImGui::TableSetupColumn("Spike", ImGuiTableColumnFlags_WidthFixed, 80);
			ImGui::TableSetupColumn("Damage /s", ImGuiTableColumnFlags_WidthFixed, 90);
			ImGui::TableSetupColumn("Enemies downed", ImGuiTableColumnFlags_WidthFixed, 120);
			ImGui::TableSetupColumn("Ours downed", ImGuiTableColumnFlags_WidthFixed, 100);
			Headers({{"Time", nullptr}, {"Spike", "Whose damage peaked"}, {"Damage /s", "At the peak second"},
				{"Enemies downed", "From 1 s before to 4 s after"}, {"Ours downed", "From 1 s before to 4 s after"}});
			struct Row { int64_t T; bool Ours; };
			std::vector<Row> rows;
			for (int64_t t : f.OurSpikesMs) { rows.push_back({t, true}); }
			for (int64_t t : f.TheirSpikesMs) { rows.push_back({t, false}); }
			std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.T < b.T; });
			for (const Row& r : rows)
			{
				size_t sec = static_cast<size_t>(r.T / 1000);
				int64_t dmg = r.Ours ? (sec < f.OutPerS.size() ? f.OutPerS[sec] : 0) : (sec < f.InPerS.size() ? f.InPerS[sec] : 0);
				ImGui::TableNextRow();
				Cell(Duration(r.T));
				ImGui::TableNextColumn();
				Key(r.Ours ? kYou : kEnemy, r.Ours ? "ours" : "enemy");
				NumCell(Num(double(dmg)));
				NumCell(std::to_string(DownsNear(f.EnemyDownMs, r.T - 1000, r.T + 4000)));
				NumCell(std::to_string(DownsNear(f.SquadDownMs, r.T - 1000, r.T + 4000)));
			}
			ImGui::EndTable();
		}
	}

	void FightTab(const Ctx& c)
	{
		const Fight& f = *c.F;
		State& s = S();
		if (!c.OneRound) { ImGui::TextColored(kMuted, "The time line shows the round picked at the top; Tonight doesn't apply here."); }

		// The answer: the enemy spike that downed the most of ours
		int64_t worstT = -1;
		int worstN = 0;
		for (int64_t t : f.TheirSpikesMs)
		{
			int n = DownsNear(f.SquadDownMs, t - 1000, t + 4000);
			if (n > worstN) { worstN = n; worstT = t; }
		}
		int ourSpikeDowns = 0;
		for (int64_t t : f.OurSpikesMs) { ourSpikeDowns += DownsNear(f.EnemyDownMs, t - 1000, t + 4000); }
		std::string answer = worstT >= 0 ? "The enemy spike at " + Duration(worstT) + " downed " + std::to_string(worstN) + " of ours"
			: "No enemy spike downed any of ours";
		answer += "; our " + std::to_string(f.OurSpikesMs.size()) + " spikes downed " + std::to_string(ourSpikeDowns) + " of theirs (" +
			std::to_string(f.EnemyDowns) + " in the whole round).";
		Answer(answer);
		ImGui::Checkbox("Table", &s.FightTable);
		if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The same, spike by spike"); }
		if (s.FightTable) { SpikeTable(f); return; }
		ImGui::SameLine(0, 20);
		Key(kYou, "our damage /s");
		Key(kEnemy, "enemy damage /s");
		Key(kOurBand, "our spike");
		Key(kEnemyBand, "enemy spike");
		ImGui::NewLine();

		float lh = ImGui::GetTextLineHeight();
		float width = std::max(300.0f, ImGui::GetContentRegionAvail().x - kLabelW - 8);
		float graphH = lh * 11;
		float x0 = ImGui::GetWindowPos().x - ImGui::GetScrollX() + kLabelW; // SameLine(x) is from the window's edge

		Track("Enemies downed");
		DownMarks(f, f.EnemyDownMs, x0, width, false);

		// Side labels: totals and the scale
		ImVec2 top = ImGui::GetCursorScreenPos();
		double mx = 0;
		for (int64_t v : f.OutPerS) { mx = std::max(mx, double(v)); }
		for (int64_t v : f.InPerS) { mx = std::max(mx, double(v)); }
		mx = NiceMax(mx);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
		double secs = std::max(1.0, f.DurationMs / 1000.0);
		dl->AddText(ImVec2(top.x, top.y), ink, "Our damage /s");
		SmallText(dl, ImVec2(top.x, top.y + lh), muted, Num(double(f.SquadDamage)) + ", " + Num(f.SquadDamage / secs) + " /s");
		dl->AddText(ImVec2(top.x, top.y + graphH - 2 * lh), ink, "Enemy damage /s");
		SmallText(dl, ImVec2(top.x, top.y + graphH - lh), muted, Num(double(f.EnemyDamage)) + ", " + Num(f.EnemyDamage / secs) + " /s");
		for (int i = 0; i < 5; i++)
		{
			std::string label = i == 2 ? "0" : Num(mx * std::abs(2 - i) / 2);
			float w = ImGui::CalcTextSize(label.c_str()).x * 0.85f;
			float y = top.y + graphH * i / 4 - (i == 0 ? 0 : i == 4 ? lh : lh * 0.5f);
			SmallText(dl, ImVec2(x0 - w - 6, y), muted, label);
		}

		// The graph
		ImGui::SetCursorScreenPos(ImVec2(x0, top.y));
		ImVec2 g = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("graph", ImVec2(width, graphH));
		bool hovered = ImGui::IsItemHovered();
		double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
		auto x = [&](double aMs) { return g.x + static_cast<float>(std::clamp(aMs / span, 0.0, 1.0)) * width; };
		float mid = g.y + graphH * 0.5f;
		dl->AddRectFilled(g, ImVec2(g.x + width, g.y + graphH), kLaneBg);
		for (int64_t t : f.OurSpikesMs) { dl->AddRectFilled(ImVec2(x(t - 2000.0), g.y), ImVec2(x(t + 2000.0), g.y + graphH), kOurBand); }
		for (int64_t t : f.TheirSpikesMs) { dl->AddRectFilled(ImVec2(x(double(t)), g.y), ImVec2(x(t + 3000.0), g.y + graphH), kEnemyBand); }
		for (float q : {0.25f, 0.75f}) { dl->AddLine(ImVec2(g.x, g.y + graphH * q), ImVec2(g.x + width, g.y + graphH * q), IM_COL32(36, 39, 44, 255)); }
		size_t n = std::max(f.OutPerS.size(), f.InPerS.size());
		float bw = std::max(1.0f, width / std::max<size_t>(1, n) * 0.72f);
		for (size_t sidx = 0; sidx < n; sidx++)
		{
			float bx = x(sidx * 1000.0);
			if (sidx < f.OutPerS.size()) { Rect(dl, ImVec2(bx, mid - static_cast<float>(f.OutPerS[sidx] / mx) * graphH * 0.5f), bw, static_cast<float>(f.OutPerS[sidx] / mx) * graphH * 0.5f, kYou); }
			if (sidx < f.InPerS.size()) { Rect(dl, ImVec2(bx, mid), bw, static_cast<float>(f.InPerS[sidx] / mx) * graphH * 0.5f, kEnemy); }
		}
		dl->AddLine(ImVec2(g.x, mid), ImVec2(g.x + width, mid), IM_COL32(58, 62, 69, 255));

		// Readout for the second under the mouse
		if (hovered)
		{
			float mxp = ImGui::GetIO().MousePos.x;
			int sec = static_cast<int>((mxp - g.x) / width * span / 1000.0);
			sec = std::clamp(sec, 0, static_cast<int>(n ? n - 1 : 0));
			float cx = x(sec * 1000.0 + 500.0);
			dl->AddLine(ImVec2(cx, g.y), ImVec2(cx, g.y + graphH), ink);
			ImGui::BeginTooltip();
			ImGui::Text("%s", Duration(sec * 1000LL).c_str());
			ImGui::Text("%s our damage /s", Num(sec < static_cast<int>(f.OutPerS.size()) ? double(f.OutPerS[sec]) : 0.0).c_str());
			ImGui::Text("%s enemy damage /s", Num(sec < static_cast<int>(f.InPerS.size()) ? double(f.InPerS[sec]) : 0.0).c_str());
			ImGui::Text("%d of ours downed, %d of theirs", DownsNear(f.SquadDownMs, sec * 1000LL, sec * 1000LL + 999), DownsNear(f.EnemyDownMs, sec * 1000LL, sec * 1000LL + 999));
			auto castsAt = [&](const Player* p)
			{
				std::string out;
				if (!p) { return out; }
				for (auto& [sk, r] : p->Skills)
				{
					for (int32_t t : r.CastMs)
					{
						if (t / 1000 == sec) { out += (out.empty() ? "" : ", ") + c.Name(sk); break; }
					}
				}
				return out;
			};
			std::string mine = castsAt(c.MeRaw), theirs = castsAt(c.VsRaw);
			if (!mine.empty()) { ImGui::TextColored(kMuted, "You cast %s", mine.c_str()); }
			if (!theirs.empty() && c.VsRaw) { ImGui::TextColored(kMuted, "%s cast %s", c.VsRaw->Name.c_str(), theirs.c_str()); }
			ImGui::EndTooltip();
		}

		Track("Ours downed");
		DownMarks(f, f.SquadDownMs, x0, width, true);
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y));
		TimeAxis(f, x0, width);

		// You and the compared player on the same time line
		ImGui::Spacing();
		if (!c.MeRaw) { return; }
		const Metric& m = Metrics()[c.RoleMetric];
		ImGui::TextColored(kMuted, "You%s on the same time line (%s skills)", c.VsRaw ? (" and " + c.VsRaw->Name).c_str() : "", m.Name.c_str());
		std::vector<int32_t> mine = CastTimes(*c.MeRaw, c.RoleMetric);
		std::string label = "Your casts (" + std::to_string(mine.size()) + ")";
		Track(label.c_str());
		Lane(f, mine, kYou, width, lh * 0.8f, TimingWindow(c.RoleMetric));
		if (c.VsRaw)
		{
			std::vector<int32_t> theirs = CastTimes(*c.VsRaw, c.RoleMetric);
			label = c.VsRaw->Name + " (" + std::to_string(theirs.size()) + ")";
			Track(label.c_str());
			Lane(f, theirs, kPeerTick, width, lh * 0.8f, TimingWindow(c.RoleMetric));
		}
		int32_t stabMs = 0;
		for (auto& [a, b] : c.MeRaw->StabOnMe) { stabMs += b - a; }
		label = "Stability on you (" + Num(stabMs / 1000.0) + " s)";
		Track(label.c_str());
		Spans(f, c.MeRaw->StabOnMe, kYou, width, lh * 0.8f);
		std::vector<std::pair<int32_t, int32_t>> cc, down, dead;
		for (int32_t t : c.MeRaw->CcMs) { cc.push_back({t, t + 700}); }
		for (const Analysis::Span& sp : c.MeRaw->DownSpans) { (sp.Dead ? dead : down).push_back({sp.From, sp.To}); }
		label = "You: CC " + std::to_string(cc.size()) + ", downed " + Num(c.MeRaw->DownedMs / 1000.0) + " s";
		Track(label.c_str());
		ImVec2 p = ImGui::GetCursorScreenPos();
		Spans(f, down, kPeerTick, width, lh * 0.8f);
		ImDrawList* d2 = ImGui::GetWindowDrawList();
		for (auto& [a, b] : dead) { d2->AddRectFilled(ImVec2(x(a), p.y + 1), ImVec2(x(b), p.y + lh * 0.8f - 1), kPeer); }
		for (auto& [a, b] : cc) { d2->AddRectFilled(ImVec2(x(a), p.y + 1), ImVec2(std::max(x(b), x(a) + 2), p.y + lh * 0.8f - 1), kEnemy); }
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lh * 0.8f + ImGui::GetStyle().ItemSpacing.y));
		Key(kEnemy, "CC");
		Key(kPeerTick, "downed");
		Key(kPeer, "dead");
		ImGui::NewLine();
	}
}
