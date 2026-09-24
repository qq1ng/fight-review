// Squad tab (the selected round): us against the enemy, subgroups (which one held worst), then every player in
// column sets, sortable, the sorted column as bars in class colours.
#include <algorithm>
#include <cmath>
#include <limits>

#include "imgui/imgui.h"

#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		const double kUnknown = std::numeric_limits<double>::quiet_NaN();

		// A small triangle after a value: the lowest (down) or the most downs (up); the number says it too
		void Mark(bool aDown)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImGui::SameLine(0, 4);
			ImVec2 p = ImGui::GetCursorScreenPos();
			float s = ImGui::GetTextLineHeight() * 0.5f, y = p.y + ImGui::GetTextLineHeight() * 0.5f;
			ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
			if (aDown) { dl->AddTriangleFilled(ImVec2(p.x, y - s * 0.5f), ImVec2(p.x + s, y - s * 0.5f), ImVec2(p.x + s * 0.5f, y + s * 0.5f), col); }
			else { dl->AddTriangleFilled(ImVec2(p.x, y + s * 0.5f), ImVec2(p.x + s, y + s * 0.5f), ImVec2(p.x + s * 0.5f, y - s * 0.5f), col); }
			ImGui::Dummy(ImVec2(s, s));
		}

		// A number with a thin bar under it (this subgroup against the others); the lowest gets a mark
		void GroupCell(const std::string& aText, double aValue, double aMax, bool aLowest, bool aYours)
		{
			ImGui::TableNextColumn();
			ImVec2 p = ImGui::GetCursorScreenPos();
			float w = ImGui::GetContentRegionAvail().x, lh = ImGui::GetTextLineHeight();
			float tw = ImGui::CalcTextSize(aText.c_str()).x + (aLowest ? lh * 0.5f + 4 : 0);
			if (w > tw) { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - tw); }
			ImGui::TextUnformatted(aText.c_str());
			if (aLowest) { Mark(true); }
			if (aMax > 0 && aValue > 0)
			{
				float bw = static_cast<float>(w * std::min(1.0, aValue / aMax));
				Rect(ImGui::GetWindowDrawList(), ImVec2(p.x + w - bw, p.y + lh + 1), bw, 2, aYours ? kYou : kPeer);
			}
		}

		struct Column
		{
			const char* Label;
			const char* Tip;
			std::function<double(const Fight&, const Player&)> Value; // NaN = unknown
			std::function<std::string(const Fight&, const Player&, double)> Text;
			unsigned Sets;
		};
		enum Set { S_Support = 1, S_Damage = 2, S_Defence = 4, S_Boons = 8 };
		const char* kSetNames[] = {"Support", "Damage", "Defence", "Boons", "All"};

		std::string Pct(double v) { return std::to_string(int(v + 0.5)) + "%"; }

		const std::vector<Column>& Columns()
		{
			static std::vector<Column> cols = []
			{
				auto num = [](const Fight&, const Player&, double v) { return Num(v); };
				auto whole = [](const Fight&, const Player&, double v) { return std::to_string(static_cast<long long>(v)); };
				std::vector<Column> v;
				v.push_back({"Healing /s", "Healing Stats, per second alive", [](const Fight&, const Player& p) { return p.HealKnown ? PerS(p, double(p.Heal)) : kUnknown; }, num, S_Support});
				v.push_back({"Barrier /s", "Barrier given, per second", [](const Fight&, const Player& p) { return p.HealKnown ? PerS(p, double(p.Barrier)) : kUnknown; }, num, S_Support});
				v.push_back({"Cleanses", "Ally conditions removed", [](const Fight&, const Player& p) { return double(p.Cleanses); }, whole, S_Support});
				v.push_back({"CC covered", "Subgroup CC your stability covered", [](const Fight&, const Player& p) { return p.StabAllyMs > 0 && p.StabEligible >= 3 ? double(p.StabCovered) / p.StabEligible : -1.0; },
					[](const Fight&, const Player& p, double v) { return v < 0 ? std::string("-") : Share(p.StabCovered, p.StabEligible, 3); }, S_Support});
				v.push_back({"Ready", "Covered CC with 3+ s left", [](const Fight&, const Player& p) { return p.StabAllyMs > 0 && p.StabCovered ? double(p.StabReady) / p.StabCovered : -1.0; },
					[](const Fight&, const Player& p, double v) { return v < 0 ? std::string("-") : Share(p.StabReady, p.StabCovered); }, S_Support});
				v.push_back({"Damage /s", "To enemy players, per second", [](const Fight&, const Player& p) { return PerS(p, double(p.Damage)); }, num, S_Damage});
				v.push_back({"Damage all /s", "To anything hostile", [](const Fight&, const Player& p) { return PerS(p, double(p.DamageAll)); }, num, S_Damage});
				v.push_back({"Strips", "Enemy boons removed", [](const Fight&, const Player& p) { return double(p.Strips); }, whole, S_Damage | S_Support});
				v.push_back({"Taken /s", "Damage taken per second", [](const Fight&, const Player& p) { return PerS(p, double(p.DamageTaken)); }, num, S_Defence});
				v.push_back({"Evades", "Enemy hits evaded", [](const Fight&, const Player& p) { return double(p.Evades); }, whole, S_Defence});
				v.push_back({"Blocks", "Enemy hits blocked", [](const Fight&, const Player& p) { return double(p.Blocks); }, whole, S_Defence});
				v.push_back({"Invulns", "Enemy hits absorbed", [](const Fight&, const Player& p) { return double(p.Invulns); }, whole, S_Defence});
				v.push_back({"CC taken", "Times crowd controlled", [](const Fight&, const Player& p) { return double(p.CcTaken); }, whole, S_Defence});
				for (int b = 0; b < Analysis::kBoons; b++)
				{
					unsigned sets = S_Boons | (b == 2 || b == 7 || b == Analysis::kStability ? S_Support : 0) | (b == 0 ? S_Damage : 0);
					v.push_back({Analysis::kBoonNames[b], "Given to own subgroup", [b](const Fight& f, const Player& p) { return f.GroupGeneration(p, b); },
						[b](const Fight& f, const Player&, double x) { return f.Intensity[b] ? Num(x) : Pct(x); }, sets});
				}
				v.push_back({"Downed", "Times downed", [](const Fight&, const Player& p) { return double(p.Downs); }, whole, S_Support | S_Damage | S_Defence | S_Boons});
				v.push_back({"Died", "Times died", [](const Fight&, const Player& p) { return double(p.Deaths); }, whole, S_Support | S_Damage | S_Defence | S_Boons});
				return v;
			}();
			return cols;
		}

		void UsAndEnemy(const Fight& f)
		{
			if (!ImGui::BeginTable("round", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) { return; }
			const char* heads[] = {"", "Players", "Downed", "Killed", "Damage", "Spikes"};
			for (const char* h : heads) { ImGui::TableSetupColumn(h, ImGuiTableColumnFlags_WidthFixed, h[0] ? 70.0f : 60.0f); }
			Headers({{"", nullptr}, {"Players", nullptr}, {"Downed", "Of this side"}, {"Killed", "Of this side"}, {"Damage", "Dealt by this side"},
				{"Spikes", "Damage peaks"}});
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); Key(kYou, "Us");
			NumCell(std::to_string(f.SquadCount)); NumCell(std::to_string(f.SquadDowns)); NumCell(std::to_string(f.SquadDeaths));
			NumCell(Num(double(f.SquadDamage))); NumCell(std::to_string(f.OurSpikesMs.size()));
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); Key(kEnemy, "Enemy");
			NumCell(std::to_string(f.EnemyCount)); NumCell(std::to_string(f.EnemyDowns)); NumCell(std::to_string(f.EnemyDeaths));
			NumCell(Num(double(f.EnemyDamage))); NumCell(std::to_string(f.TheirSpikesMs.size()));
			ImGui::EndTable();
		}

		void Subgroups(const Ctx& c)
		{
			const Fight& f = *c.F;
			State& s = S();
			std::map<int, std::vector<const Player*>> members;
			for (const Player& p : f.Players) { members[p.Subgroup].push_back(&p); }
			auto cover = [&](int g) { auto w = f.GroupCcWindows.find(g); auto k = f.GroupCcCovered.find(g);
				int n = w == f.GroupCcWindows.end() ? 0 : w->second, m = k == f.GroupCcCovered.end() ? 0 : k->second; return std::pair<int, int>(m, n); };
			auto downs = [&](int g) { int d = 0; for (const Player* p : members[g]) { d += p->Downs; } return d; };
			auto deaths = [&](int g) { int d = 0; for (const Player* p : members[g]) { d += p->Deaths; } return d; };

			// The answer: the subgroup whose CC stability covered least (with enough CC to judge), else the most downs
			int worst = -1;
			double worstShare = 2;
			for (auto& [g, list] : members)
			{
				auto [m, n] = cover(g);
				if (n >= 3 && double(m) / n < worstShare) { worstShare = double(m) / n; worst = g; }
			}
			if (worst >= 0)
			{
				auto [m, n] = cover(worst);
				Answer("Subgroup " + std::to_string(worst) + " held worst: stability covered " + Share(m, n) + " of its CC, and it had " +
					std::to_string(downs(worst)) + " of our " + std::to_string(f.SquadDowns) + " downs.");
			}
			else { Answer("Too little CC this round to judge stability by subgroup."); }

			std::vector<int> boons = {Analysis::kStability, 7, 2, 4, 11, 3}; // stability, aegis, quickness, protection, resolution, alacrity
			if (s.AllBoons) { boons.clear(); for (int b = 0; b < Analysis::kBoons; b++) { boons.push_back(b); } }
			ImGui::Checkbox("All boons", &s.AllBoons);
			ImGui::SameLine(0, 20);
			ImGui::TextColored(kMuted, "Marked: the lowest in each column, the most downs.");
			int cols = 3 + static_cast<int>(boons.size()) + 2;
			ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable;
			float lh = ImGui::GetTextLineHeight();
			float height = members.size() * (lh + 6) + lh + 2 * ImGui::GetStyle().CellPadding.y + 4 + (s.AllBoons ? ImGui::GetStyle().ScrollbarSize : 0);
			if (!ImGui::BeginTable("groups", cols, flags, ImVec2(0, height))) { return; }
			ImGui::TableSetupScrollFreeze(2, 1);
			ImGui::TableSetupColumn("Sg", ImGuiTableColumnFlags_WidthFixed, 55);
			ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 100);
			ImGui::TableSetupColumn("CC covered", ImGuiTableColumnFlags_WidthFixed, 110);
			for (int b : boons) { ImGui::TableSetupColumn(Analysis::kBoonNames[b], ImGuiTableColumnFlags_WidthFixed, 75); }
			ImGui::TableSetupColumn("Downed", ImGuiTableColumnFlags_WidthFixed, 60);
			ImGui::TableSetupColumn("Died", ImGuiTableColumnFlags_WidthFixed, 45);
			std::vector<std::pair<const char*, const char*>> heads = {{"Sg", "Subgroup"}, {"Players", nullptr}, {"CC covered", "CC with anyone's stability"}};
			for (int b : boons) { heads.push_back({Analysis::kBoonNames[b], f.Intensity[b] ? "Average stacks" : "Uptime"}); }
			heads.push_back({"Downed", nullptr}); heads.push_back({"Died", nullptr});
			Headers(heads);

			// column ranges for the bars and the marks
			double coverMax = 0, coverMin = 2;
			std::map<int, double> bMax, bMin;
			int downsMax = 0;
			for (auto& [g, list] : members)
			{
				auto [m, n] = cover(g);
				if (n >= 3) { coverMax = std::max(coverMax, double(m) / n); coverMin = std::min(coverMin, double(m) / n); }
				for (int b : boons)
				{
					auto up = f.GroupUptime.find(g);
					double v = up == f.GroupUptime.end() ? 0 : up->second[b];
					bMax[b] = std::max(bMax.count(b) ? bMax[b] : 0.0, v);
					bMin[b] = std::min(bMin.count(b) ? bMin[b] : 1e9, v);
				}
				downsMax = std::max(downsMax, downs(g));
			}
			int mine = c.MeRaw ? c.MeRaw->Subgroup : -1;
			for (auto& [g, list] : members)
			{
				bool yours = g == mine;
				ImGui::TableNextRow(0, lh + 6);
				NumCell(std::to_string(g) + (yours ? " (you)" : ""));
				ImGui::TableNextColumn();
				for (const Player* p : list) { SpecIcon(*p); }
				auto [m, n] = cover(g);
				double share = n ? double(m) / n : 0;
				GroupCell(Share(m, n), share, coverMax, n >= 3 && share == coverMin && members.size() > 1, yours);
				auto up = f.GroupUptime.find(g);
				for (int b : boons)
				{
					double v = up == f.GroupUptime.end() ? 0 : up->second[b];
					GroupCell(f.Intensity[b] ? Num(v) : Pct(v), v, bMax[b], v == bMin[b] && bMax[b] > 0 && members.size() > 1, yours);
				}
				ImGui::TableNextColumn();
				std::string d = std::to_string(downs(g));
				float w = ImGui::GetContentRegionAvail().x;
				bool most = downs(g) == downsMax && downsMax > 0;
				float tw = ImGui::CalcTextSize(d.c_str()).x + (most ? lh * 0.5f + 4 : 0);
				if (w > tw) { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - tw); }
				ImGui::TextUnformatted(d.c_str());
				if (most) { Mark(false); }
				NumCell(std::to_string(deaths(g)));
			}
			ImGui::EndTable();
		}

		void Players(const Ctx& c)
		{
			const Fight& f = *c.F;
			State& s = S();
			ImGui::TextUnformatted("Players");
			for (int i = 0; i < 5; i++)
			{
				ImGui::SameLine(0, i ? 4.0f : 16.0f);
				if (ImGui::RadioButton(kSetNames[i], s.ColumnSet == i)) { s.ColumnSet = i; }
			}
			ImGui::SameLine(0, 20);
			ImGui::TextColored(kMuted, "Click a heading to sort, a name to compare with them.");

			const auto& all = Columns();
			std::vector<int> cols;
			unsigned set = s.ColumnSet == 4 ? ~0u : (1u << s.ColumnSet);
			for (int i = 0; i < static_cast<int>(all.size()); i++) { if (all[i].Sets & set) { cols.push_back(i); } }
			ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
				ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
			float height = std::max(200.0f, ImGui::GetContentRegionAvail().y);
			if (!ImGui::BeginTable("players", 3 + static_cast<int>(cols.size()), flags, ImVec2(0, height))) { return; }
			ImGui::TableSetupScrollFreeze(3, 1);
			ImGui::TableSetupColumn("Sg", ImGuiTableColumnFlags_WidthFixed, 30, 1000);
			ImGui::TableSetupColumn("Spec", ImGuiTableColumnFlags_WidthFixed, 125, 1002);
			ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 170, 1001);
			for (size_t i = 0; i < cols.size(); i++)
			{
				ImGuiTableColumnFlags cf = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending | (i == 0 ? ImGuiTableColumnFlags_DefaultSort : 0);
				float w = std::string(all[cols[i]].Label) == "CC covered" || std::string(all[cols[i]].Label) == "Ready" ? 115.0f : 90.0f;
				ImGui::TableSetupColumn(all[cols[i]].Label, cf, w, static_cast<ImGuiID>(cols[i]));
			}
			std::vector<std::pair<const char*, const char*>> heads = {{"Sg", "Subgroup"}, {"Spec", "Specialization"}, {"Player", nullptr}};
			for (int i : cols) { heads.push_back({all[i].Label, all[i].Tip}); }
			Headers(heads);

			int sortId = cols.empty() ? 1000 : cols[0];
			bool asc = false;
			if (ImGuiTableSortSpecs* spec = ImGui::TableGetSortSpecs(); spec && spec->SpecsCount > 0)
			{
				sortId = static_cast<int>(spec->Specs[0].ColumnUserID);
				asc = spec->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
			}
			auto key = [&](const Player& p) -> double
			{
				if (sortId == 1000) { return p.Subgroup; }
				if (sortId >= 0 && sortId < static_cast<int>(all.size())) { double v = all[sortId].Value(f, p); return std::isnan(v) ? -1 : v; }
				return 0;
			};
			std::vector<const Player*> rows;
			for (const Player& p : f.Players) { rows.push_back(&p); }
			std::stable_sort(rows.begin(), rows.end(), [&](const Player* a, const Player* b)
			{
				if (sortId == 1001) { return asc ? a->Name < b->Name : a->Name > b->Name; }
				if (sortId == 1002 && a->Spec != b->Spec) { return asc ? a->Spec < b->Spec : a->Spec > b->Spec; }
				if (sortId == 1002) { return a->Name < b->Name; }
				double x = key(*a), y = key(*b);
				return asc ? x < y : x > y;
			});
			double sortedMax = 0;
			if (sortId >= 0 && sortId < static_cast<int>(all.size())) { for (const Player* p : rows) { sortedMax = std::max(sortedMax, key(*p)); } }
			for (const Player* p : rows)
			{
				ImGui::TableNextRow();
				NumCell(std::to_string(p->Subgroup));
				ImGui::TableNextColumn();
				SpecIcon(*p);
				ImGui::TextUnformatted(p->Spec.c_str());
				ImGui::TableNextColumn();
				std::string name = p->Name + (p->Pov ? " (you)" : "");
				if (ImGui::Selectable(name.c_str(), false) && !p->Pov) { S().VsAccount = p->Account; }
				if (ImGui::IsItemHovered() && !p->Pov) { ImGui::SetTooltip("Compare yourself with %s", p->Name.c_str()); }
				for (int i : cols)
				{
					double v = all[i].Value(f, *p);
					std::string text = std::isnan(v) ? "unknown" : all[i].Text(f, *p, v);
					if (i == sortId && !std::isnan(v) && v >= 0)
					{
						ImU32 col = ImGui::ColorConvertFloat4ToU32(ProfessionColor(p->ProfId)) & 0x99FFFFFF;
						BarCell(v, sortedMax, col, text, p->Pov);
					}
					else if (std::isnan(v)) { Cell(text, &kMuted); }
					else { NumCell(text); }
				}
			}
			ImGui::EndTable();
		}
	}

	void SquadTab(const Ctx& c)
	{
		if (!c.OneRound) { ImGui::TextColored(kMuted, "Squad shows the round picked at the top; Tonight doesn't apply here."); }
		UsAndEnemy(*c.F);
		ImGui::Spacing();
		Subgroups(c);
		ImGui::Spacing();
		Players(c);
	}
}
