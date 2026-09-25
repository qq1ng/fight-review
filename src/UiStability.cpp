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

	void StabilityGivers(const Ctx& c)
	{
		const Fight& f = *c.F;
		std::vector<const Player*> givers;
		for (const Player& p : f.Players) { if (p.StabAllyMs > 0 && f.GroupGeneration(p, Analysis::kStability) >= 0.1) { givers.push_back(&p); } }
		std::sort(givers.begin(), givers.end(), [&](const Player* a, const Player* b)
			{ return a->Subgroup != b->Subgroup ? a->Subgroup < b->Subgroup : f.GroupGeneration(*a, Analysis::kStability) > f.GroupGeneration(*b, Analysis::kStability); });
		if (givers.empty()) { ImGui::TextColored(kMuted, "Nobody gave stability to others this round."); return; }
		ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
		if (!ImGui::BeginTable("stabgivers", 7, flags)) { return; }
		const char* heads[] = {"Sg", "Giver", "CC covered", "On subgroup", "Lost to enemy", "Casts ahead of spike", "Main skills"};
		float widths[] = {30, 170, 110, 90, 95, 140, 285};
		for (int i = 0; i < 7; i++) { ImGui::TableSetupColumn(heads[i], ImGuiTableColumnFlags_WidthFixed, widths[i]); }
		Headers({{"Sg", "Subgroup"}, {"Giver", nullptr}, {"CC covered", "Subgroup CC hitting their stability"},
			{"On subgroup", "Average stacks they kept on it"}, {"Lost to enemy", "Their stacks stripped or used up"},
			{"Casts ahead of spike", "In the 4 s before"}, {"Main skills", "Most of their stability came from"}});
		for (const Player* p : givers)
		{
			ImGui::TableNextRow();
			NumCell(std::to_string(p->Subgroup));
			ImGui::TableNextColumn();
			SpecIcon(*p);
			ImGui::TextUnformatted((p->Name + (p->Pov ? " (you)" : "")).c_str());
			NumCell(Share(p->StabCovered, p->StabEligible, 3));
			NumCell(Num(f.GroupGeneration(*p, Analysis::kStability)));
			NumCell(std::to_string(p->StabGivenLost));
			auto skills = StabSkills(*p);
			int casts = 0, ahead = 0;
			std::string main;
			for (size_t i = 0; i < skills.size(); i++)
			{
				const SkillRow& r = p->Skills.at(skills[i].first);
				casts += r.Casts; ahead += r.Timing[Analysis::T_AheadOfTheirs];
				if (i < 3 && skills[i].first != 0) { main += (main.empty() ? "" : ", ") + c.Name(skills[i].first); }
			}
			NumCell(casts ? std::to_string(ahead) + " of " + std::to_string(casts) : "-");
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
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddTriangleFilled(ImVec2(p.x + 1, p.y + 2), ImVec2(p.x + 11, p.y + 2), ImVec2(p.x + 6, p.y + l * 0.6f), kEnemy); }, "CC landed");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddTriangleFilled(ImVec2(p.x + 1, p.y + l - 2), ImVec2(p.x + 11, p.y + l - 2), ImVec2(p.x + 6, p.y + l * 0.4f), kEnemy); }, "stripped");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x + 5, p.y + l * 0.55f), ImVec2(p.x + 7, p.y + l - 1), kPeerTick); }, "stack used up");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x + 5, p.y + 1), ImVec2(p.x + 6, p.y + l - 1), kPeerTick); }, "ran out");
			item([](ImDrawList* d, ImVec2 p, float l) { d->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 12, p.y + l - 2), kPeer); }, "downed or dead");
			ImGui::NewLine();
		}
		const float labelW = 200, laneW = std::max(250.0f, ImGui::GetContentRegionAvail().x - labelW - 8);
		const float h = ImGui::GetTextLineHeight() * 1.1f;
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
			for (int64_t t : f.TheirSpikesMs) { dl->AddRect(ImVec2(x(pos, t - 4000.0), pos.y + 1), ImVec2(x(pos, double(t)), pos.y + h - 1), kEnemy); }
			for (int64_t t : f.TheirSpikesMs) { dl->AddRectFilled(ImVec2(x(pos, double(t)), pos.y + 1), ImVec2(x(pos, t + 3000.0), pos.y + h - 1), kEnemy); }
			ImGui::Dummy(ImVec2(laneW, h));
		}
		ImGui::TextColored(kMuted, "Our spikes");
		ImGui::SameLine(labelW);
		{
			ImVec2 pos = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(pos, ImVec2(pos.x + laneW, pos.y + h), kLaneBg);
			for (int64_t t : f.OurSpikesMs) { dl->AddRectFilled(ImVec2(x(pos, t - 2000.0), pos.y + 1), ImVec2(x(pos, t + 2000.0), pos.y + h - 1), kYou); }
			ImGui::Dummy(ImVec2(laneW, h));
		}
		// Stability given to the subgroup: one lane, a tick per cast or pulse that reached someone in it (taller when it
		// reached more of them); who gave it, with which skill and whom it reached or missed, on hover (the user,
		// 2026-09-24: a lane per giver was too much)
		{
			std::vector<int> inGroup;
			for (const Player* p : members[group]) { inGroup.push_back(static_cast<int>(p - f.Players.data())); }
			struct Tick { int32_t Ms; const Player* By; const Player::StabGive* Give; int Reached; };
			std::vector<Tick> ticks;
			for (const Player& p : f.Players)
			{
				for (const auto& g : p.StabGives)
				{
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
			for (int32_t t : p->CcMs) { float cx = x(pos, t); dl->AddTriangleFilled(ImVec2(cx - 4, pos.y), ImVec2(cx + 4, pos.y), ImVec2(cx, pos.y + h * 0.55f), kEnemy); }
			for (auto& [t, kind] : p->StabLost)
			{
				float cx = x(pos, t);
				if (kind == 0) { dl->AddTriangleFilled(ImVec2(cx - 4, pos.y + h), ImVec2(cx + 4, pos.y + h), ImVec2(cx, pos.y + h * 0.45f), kEnemy); }
				else { dl->AddRectFilled(ImVec2(cx, pos.y + h * 0.6f), ImVec2(cx + 2, pos.y + h), kPeerTick); }
			}
			for (auto& [a, b] : p->BoonOn[Analysis::kStability])
			{
				if (b >= f.DurationMs - 500) { continue; }
				bool taken = false;
				for (auto& [t, kind] : p->StabLost) { taken |= std::abs(t - b) <= 150; }
				for (const Analysis::Span& d : p->DownSpans) { taken |= std::abs(d.From - b) <= 150; }
				if (!taken) { float cx = x(pos, b); dl->AddRectFilled(ImVec2(cx, pos.y), ImVec2(cx + 1, pos.y + h), kPeerTick); }
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
