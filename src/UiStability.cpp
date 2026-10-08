// Stability: who gave stability and how well timed (Squad), and a time line of
// one subgroup: each member's stability, the CC that landed, downs, and the stability given (Round).
#include <algorithm>
#include <map>

#include "imgui/imgui.h"

#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		double Covered(const std::vector<std::pair<int32_t, int32_t>>& aSpans, int64_t aDuration)
		{
			double ms = 0;
			for (auto& [a, b] : aSpans) { ms += b - a; }
			return aDuration > 0 ? 100.0 * ms / aDuration : 0.0;
		}

		bool StabBefore(const Player& p, int32_t aMs)
		{
			for (auto& [a, b] : p.BoonOn[Analysis::kStability]) { if (a <= aMs && b >= aMs - 3000) { return true; } }
			return false;
		}

		// Skills that gave this player's stability to the subgroup, most first
		std::vector<std::pair<int32_t, double>> StabSkills(const Player& p)
		{
			std::vector<std::pair<int32_t, double>> out;
			for (auto& [sk, r] : p.Skills) { if (r.BoonGroupS[Analysis::kStability] > 0) { out.push_back({sk, r.BoonGroupS[Analysis::kStability]}); } }
			std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second > b.second; });
			return out;
		}

	}

	// The stability score: TopStats' "Stability Performance" score (WvW Insights' guide: 0.25 amount + 0.30 ready + 0.30
	// coverage, over 0.85; without ready, 0.25 amount + 0.30 coverage over 0.55; 3+ CC windows on the subgroup needed),
	// where amount is stability given per minute alive against the round's most. Beside it, what the month of logs says
	// matters most: the giver's subgroup with stability when enemy spikes hit.
	struct StabScore { double Score = -1, Amount = 0, AmountMax = 0, AmountPts = 0, Ready = -1, Coverage = -1, AtSpikes = -1, Redundancy = -1; };
	StabScore ScoreOf(const Fight& f, const Player& p, double aAmountMax)
	{
		StabScore s;
		s.Amount = p.ActiveMs > 0 ? p.StabAllyMs / 1000.0 / (p.ActiveMs / 60000.0) : 0;
		s.AmountMax = aAmountMax;
		s.AmountPts = aAmountMax > 0 ? 100 * s.Amount / aAmountMax : 0;
		if (p.StabCovered > 0) { s.Ready = 100.0 * p.StabReady / p.StabCovered; }
		if (p.StabEligible >= 3) { s.Coverage = 100.0 * p.StabCovered / p.StabEligible; }
		if (s.Coverage >= 0) { s.Score = s.Ready >= 0 ? (0.25 * s.AmountPts + 0.30 * s.Ready + 0.30 * s.Coverage) / 0.85 : (0.25 * s.AmountPts + 0.30 * s.Coverage) / 0.55; }
		if (p.StabAllyNominalMs > 0) { s.Redundancy = 100.0 * p.StabRedundantMs / p.StabAllyNominalMs; }
		// the subgroup at enemy spikes: members hit in it, up at its peak, with stability (anyone's) then
		int hit = 0, with = 0;
		for (int64_t t : f.TheirSpikesMs)
		{
			auto [a, b] = SpikeWindow(f, false, t, 1500, 1500);
			for (const Player& m : f.Players)
			{
				if (m.Subgroup != p.Subgroup) { continue; }
				if (!std::any_of(m.HitsIn.begin(), m.HitsIn.end(), [&](const auto& h) { return h.Ms >= a && h.Ms <= b; })) { continue; }
				if (std::any_of(m.DownSpans.begin(), m.DownSpans.end(), [&](const auto& d) { return d.From <= t && d.To >= t; })) { continue; }
				hit++;
				with += std::any_of(m.BoonOn[Analysis::kStability].begin(), m.BoonOn[Analysis::kStability].end(), [&](const auto& s2) { return s2.first <= t && s2.second >= t; });
			}
		}
		if (hit) { s.AtSpikes = 100.0 * with / hit; }
		return s;
	}

	void StabilityGivers(const Ctx& c)
	{
		const Fight& f = *c.F;
		std::vector<const Player*> givers;
		for (const Player& p : f.Players) { if (p.StabAllyMs > 0 && f.GroupGeneration(p, Analysis::kStability) >= 0.1) { givers.push_back(&p); } }
		std::sort(givers.begin(), givers.end(), [&](const Player* a, const Player* b)
			{ return a->Subgroup != b->Subgroup ? a->Subgroup < b->Subgroup : f.GroupGeneration(*a, Analysis::kStability) > f.GroupGeneration(*b, Analysis::kStability); });
		if (givers.empty()) { ImGui::TextColored(kMuted, "Nobody gave stability to others this round."); return; }
		double amountMax = 0;
		for (const Player* p : givers) { if (p->ActiveMs > 0) { amountMax = std::max(amountMax, p->StabAllyMs / 1000.0 / (p->ActiveMs / 60000.0)); } }
		ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
		if (!ImGui::BeginTable("stabgivers", 8, flags)) { return; }
		const char* heads[] = {"Sg", "Giver", "Score", "CC covered", "Ready", "At enemy spikes", "Redundancy", "Main skills"};
		float widths[] = {28, 150, 50, 115, 50, 110, 85, 0};
		for (int i = 0; i < 8; i++) { ImGui::TableSetupColumn(heads[i], i == 7 ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed, widths[i]); }
		Headers({{"Sg", "Subgroup"}, {"Giver", nullptr}, {"Score", "Amount, ready and coverage"}, {"CC covered", "Subgroup CC hitting their stability"},
			{"Ready", "Covered with 3+ s left"}, {"At enemy spikes", "Their subgroup with stability"}, {"Redundancy", "On another giver's stacks"},
			{"Main skills", "Most of their stability came from"}});
		auto pct = [](double v) { return v < 0 ? std::string("-") : std::to_string(static_cast<int>(v + 0.5)) + "%"; };
		for (const Player* p : givers)
		{
			const StabScore sc = ScoreOf(f, *p, amountMax);
			ImGui::TableNextRow();
			NumCell(std::to_string(p->Subgroup));
			ImGui::TableNextColumn();
			SpecIcon(*p);
			ImGui::TextUnformatted((p->Name + (p->Pov ? " (you)" : "")).c_str());
			NumCell(sc.Score < 0 ? std::string("-") : std::to_string(static_cast<int>(sc.Score + 0.5)));
			if (ImGui::IsItemHovered())
			{
				TipWrapped(sc.Score < 0 ? std::string("Under 3 CC windows on their subgroup: too few to score.")
					: "Score " + std::to_string(static_cast<int>(sc.Score + 0.5)) + " (TopStats' stability score): amount " + std::to_string(static_cast<int>(sc.AmountPts + 0.5)) +
					" (" + Num(sc.Amount) + " stack-seconds a minute; the round's most " + Num(sc.AmountMax) + "), ready " + pct(sc.Ready) + ", CC covered " + pct(sc.Coverage) +
					". Redundancy isn't part of it.");
			}
			NumCell(Share(p->StabCovered, p->StabEligible, 3));
			NumCell(pct(sc.Ready));
			NumCell(pct(sc.AtSpikes));
			NumCell(pct(sc.Redundancy));
			auto skills = StabSkills(*p);
			std::string main;
			for (size_t i = 0; i < skills.size() && i < 3; i++) { if (skills[i].first != 0) { main += (main.empty() ? "" : ", ") + c.Name(skills[i].first); } }
			Cell(main.empty() ? "other sources" : main);
		}
		ImGui::EndTable();
	}

	void StabilityTimeLine(const Ctx& c)
	{
		const Fight& f = *c.F;
		State& s = S();
		std::map<int, std::vector<const Player*>> members;
		for (const Player& p : f.Players) { members[p.Subgroup].push_back(&p); }
		int group = s.StabGroup >= 0 && members.count(s.StabGroup) ? s.StabGroup : c.MeRaw ? c.MeRaw->Subgroup : members.begin()->first;
		ImGui::TextUnformatted("Subgroup over time");
		ImGui::SameLine(0, 12);
		ImGui::SetNextItemWidth(110);
		if (ImGui::BeginCombo("##stabgroup", ("Subgroup " + std::to_string(group)).c_str()))
		{
			for (auto& [g, list] : members) { if (ImGui::Selectable(("Subgroup " + std::to_string(g)).c_str(), g == group)) { s.StabGroup = g; } }
			ImGui::EndCombo();
		}
		ImGui::SameLine(0, 20);
		ImGui::NewLine();
		// The legend: shapes, not only colours (orange is the enemy: spikes are blocks, CC and strips are triangles)
		{
			ImDrawList* ld = ImGui::GetWindowDrawList();
			float lh2 = ImGui::GetTextLineHeight();
			auto item = [&](auto aDraw, const char* aText)
			{
				// wrap to the next line when it doesn't fit
				if (ImGui::GetContentRegionAvail().x < 18 + ImGui::CalcTextSize(aText).x) { ImGui::NewLine(); }
				ImVec2 p = ImGui::GetCursorScreenPos();
				aDraw(ld, p, lh2);
				ImGui::Dummy(ImVec2(14, lh2));
				ImGui::SameLine(0, 4);
				ImGui::TextColored(kMuted, "%s", aText);
				ImGui::SameLine(0, 14);
			};
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x, p.y + l * 0.3f), ImVec2(p.x + 12, p.y + l * 0.7f), kEnemy); }, "enemy spike");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRect(ImVec2(p.x, p.y + l * 0.2f), ImVec2(p.x + 12, p.y + l * 0.8f), kEnemy); }, "4 s before it");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x + 5, p.y + 1), ImVec2(p.x + 7, p.y + l - 1), kYou); }, "stability given (taller: reached more)");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x, p.y + l * 0.35f), ImVec2(p.x + 12, p.y + l * 0.65f), kYou); }, "stability on");
			// the same icons as Deaths: the CC type's icon, and the Stability icon outlined red when stripped or corrupted
			item([](ImDrawList* d, ImVec2 p, float l) { CcIconAt(d, ImVec2(p.x, p.y + 1), l - 2, Analysis::CC_Stun); }, "CC landed (its type)");
			item([](ImDrawList* d, ImVec2 p, float l) { BoonIconAt(d, ImVec2(p.x, p.y + 1), l - 2, Analysis::kStability, true); }, "stability stripped");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x + 5, p.y + l * 0.55f), ImVec2(p.x + 7, p.y + l - 1), kPeerTick); }, "stack used up");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x + 5, p.y + 1), ImVec2(p.x + 6, p.y + l - 1), kPeerTick); }, "ran out");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 12, p.y + l - 2), kPeer); }, "downed or dead");
			ImGui::NewLine();
		}
		const float labelW = 200, laneW = std::max(250.0f, ImGui::GetContentRegionAvail().x - labelW - 8);
		const float h = ImGui::GetTextLineHeight() * 1.35f;
		double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
		auto x = [&](ImVec2 p, double ms) { return p.x + static_cast<float>(std::clamp(ms / span, 0.0, 1.0)) * laneW; };
		// Spikes in lanes of their own (one colour each, no overlapping shades), then the givers' stability casts
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float top = ImGui::GetCursorScreenPos().y;
		// Enemy spikes: filled = the spike, outlined = the 4 s before it
		ImGui::TextColored(kMuted, "Enemy spikes");
		ImGui::SameLine(labelW);
		{
			ImVec2 pos = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(pos, ImVec2(pos.x + laneW, pos.y + h), kLaneBg);
			for (int64_t t : f.TheirSpikesMs)
			{
				auto [a, b] = SpikeWindow(f, false, t, 0, 3000);
				dl->AddRect(ImVec2(x(pos, a - 4000.0), pos.y + 1), ImVec2(x(pos, double(a)), pos.y + h - 1), kEnemy);
				dl->AddRectFilled(ImVec2(x(pos, double(a)), pos.y + 1), ImVec2(x(pos, double(b)), pos.y + h - 1), kEnemy);
			}
			ImGui::Dummy(ImVec2(laneW, h));
		}
		ImGui::TextColored(kMuted, "Ally spikes");
		ImGui::SameLine(labelW);
		{
			ImVec2 pos = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(pos, ImVec2(pos.x + laneW, pos.y + h), kLaneBg);
			for (int64_t t : f.OurSpikesMs) { auto [a, b] = SpikeWindow(f, true, t, 2000, 2000); dl->AddRectFilled(ImVec2(x(pos, double(a)), pos.y + 1), ImVec2(x(pos, double(b)), pos.y + h - 1), kYou); }
			ImGui::Dummy(ImVec2(laneW, h));
		}
		// Stability given to the subgroup: one lane, a tick per cast or pulse that reached someone in it (taller when it
		// reached more of them); who gave it, with which skill and whom it reached or missed, on hover
		{
			std::vector<int> inGroup;
			for (const Player* p : members[group]) { inGroup.push_back(static_cast<int>(p - f.Players.data())); }
			struct Tick { int32_t Ms; const Player* By; const Player::StabGive* Give; int Reached; };
			std::vector<Tick> ticks;
			for (const Player& p : f.Players)
			{
				for (const auto& g : p.StabGives)
				{
					if (g.SelfOnly) { continue; } // the caster's own stability, not given to the subgroup
					int n = 0;
					for (int t : g.Targets) { n += std::find(inGroup.begin(), inGroup.end(), t) != inGroup.end(); }
					if (n > 0) { ticks.push_back({g.Ms, &p, &g, n}); }
				}
			}
			std::sort(ticks.begin(), ticks.end(), [](const Tick& a, const Tick& b) { return a.Ms < b.Ms; });
			ImGui::TextColored(kMuted, "Stability given (%d)", static_cast<int>(ticks.size()));
			ImGui::SameLine(labelW);
			ImVec2 pos = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(pos, ImVec2(pos.x + laneW, pos.y + h), kLaneBg);
			float mx = ImGui::GetMousePos().x;
			std::vector<const Tick*> near; // the ticks under the mouse (within 5 px): several givers can cast together
			for (const Tick& t : ticks)
			{
				float cx = x(pos, t.Ms);
				float th = h * (0.35f + 0.65f * std::min(1.0f, t.Reached / float(std::max<size_t>(1, inGroup.size()))));
				dl->AddRectFilled(ImVec2(cx, pos.y + h - th), ImVec2(cx + 2, pos.y + h), kYou);
				if (std::abs(mx - cx) <= 5) { near.push_back(&t); }
			}
			ImGui::Dummy(ImVec2(laneW, h));
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				if (near.empty()) { ImGui::TextUnformatted("A tick per cast or pulse that reached the subgroup; taller when it reached more of it."); }
				for (const Tick* t : near)
				{
					std::string got, missed;
					for (const Player* m : members[group])
					{
						int idx = static_cast<int>(m - f.Players.data());
						bool hit = std::find(t->Give->Targets.begin(), t->Give->Targets.end(), idx) != t->Give->Targets.end();
						std::string& list = hit ? got : missed;
						list += (list.empty() ? "" : ", ") + m->Name;
					}
					auto name = f.SkillNames.count(t->Give->Skill) ? f.SkillNames.at(t->Give->Skill) : std::to_string(t->Give->Skill);
					std::string by = t->By->Name + (t->By->Subgroup == group ? "" : " (subgroup " + std::to_string(t->By->Subgroup) + ")");
					ImGui::Text("%s  %s: %s", Duration(t->Ms).c_str(), by.c_str(), name.c_str());
					ImGui::TextColored(kMuted, "   reached %d of %d: %s", t->Reached, static_cast<int>(inGroup.size()), got.c_str());
					if (!missed.empty()) { ImGui::TextColored(kMuted, "   missed: %s", missed.c_str()); }
				}
				ImGui::EndTooltip();
			}
		}
		ImGui::Spacing();
		ImGui::TextColored(kMuted, "Subgroup %d", group);
		for (const Player* p : members[group])
		{
			ImGui::TextUnformatted((p->Name + (p->Pov ? " (you)" : "")).c_str());
			ImGui::SameLine(labelW);
			ImVec2 pos = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(pos, ImVec2(pos.x + laneW, pos.y + h), kLaneBg);
			for (auto& [a, b] : p->BoonOn[Analysis::kStability]) { dl->AddRectFilled(ImVec2(x(pos, a), pos.y + h * 0.25f), ImVec2(x(pos, b), pos.y + h * 0.75f), kYou); }
			for (const Analysis::Span& d : p->DownSpans) { dl->AddRectFilled(ImVec2(x(pos, d.From), pos.y + 1), ImVec2(x(pos, d.To), pos.y + h - 1), kPeer); }
			// CC: a triangle on top; stability stripped: a triangle below; a stack used up: a short light tick; ran out: a
			// light line where the bar ends with nothing taking it
			for (auto& [t, kind] : p->StabLost)
			{
				if (kind != 0) { float cx = x(pos, t); dl->AddRectFilled(ImVec2(cx, pos.y + h * 0.6f), ImVec2(cx + 2, pos.y + h), kPeerTick); }
			}
			for (auto& [a, b] : p->BoonOn[Analysis::kStability])
			{
				if (b >= f.DurationMs - 500) { continue; }
				bool taken = false;
				for (auto& [t, kind] : p->StabLost) { taken |= std::abs(t - b) <= 150; }
				for (const Analysis::Span& d : p->DownSpans) { taken |= std::abs(d.From - b) <= 150; }
				if (!taken) { float cx = x(pos, b); dl->AddRectFilled(ImVec2(cx, pos.y), ImVec2(cx + 1, pos.y + h), kPeerTick); }
			}
			// icons last, on top; one that would overlap the one before it is left out (the hover gives the counts)
			{
				const float isz = h - 4;
				float last = -1e9f;
				std::vector<float> ccX;
				for (const auto& cc : p->CcIn)
				{
					float cx = x(pos, cc.Ms) - isz * 0.5f;
					if (cx - last < isz) { continue; }
					CcIconAt(dl, ImVec2(cx, pos.y + 2), isz, cc.Kind);
					ccX.push_back(cx);
					last = cx;
				}
				last = -1e9f;
				for (auto& [t, kind] : p->StabLost)
				{
					float cx = x(pos, t) - isz * 0.5f;
					for (float at : ccX) { if (std::abs(cx - at) < isz + 2) { cx = at - isz - 3; } } // strips come before the CC: to its left
					if (kind != 0 || cx - last < isz) { continue; }
					BoonIconAt(dl, ImVec2(cx, pos.y + 2), isz, Analysis::kStability, true);
					last = cx;
				}
			}
			ImGui::Dummy(ImVec2(laneW, h));
			if (ImGui::IsItemHovered())
			{
				int bare = 0;
				for (int32_t t : p->CcMs) { bare += !StabBefore(*p, t); }
				ImGui::SetTooltip("%s: stability on %d%% of the round; CC landed %d times, %d with no stability in the 3 s before; stripped %d, used up %d stacks",
					p->Name.c_str(), int(Covered(p->BoonOn[Analysis::kStability], f.DurationMs) + 0.5), static_cast<int>(p->CcMs.size()), bare, p->StabStripped, p->StabUsedUp);
			}
		}
		// A line at the mouse through every lane, so a CC or a down lines up with the spikes above it
		float bottom = ImGui::GetCursorScreenPos().y;
		float left = ImGui::GetWindowPos().x - ImGui::GetScrollX() + labelW;
		ImVec2 m = ImGui::GetMousePos();
		if (ImGui::IsWindowHovered() && m.x >= left && m.x <= left + laneW && m.y >= top && m.y <= bottom)
		{
			dl->AddLine(ImVec2(m.x, top), ImVec2(m.x, bottom), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
		}
		TimeAxis(f, left, laneW);
	}
}
