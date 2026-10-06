// Squad tab (the selected round): us against the enemy, subgroups (which one held worst), then every player in
// column sets, sortable, the sorted column as bars in class colours.
#include <algorithm>
#include <cstdio>
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
		const char* kSetNames[] = {"Support", "Damage", "Defence", "Boons", "All", "Stability givers"};
		constexpr int kStabilitySet = 5; // the givers table instead of the players

		std::string Pct(double v) { return std::to_string(int(v + 0.5)) + "%"; }

		const std::vector<Column>& Columns()
		{
			static std::vector<Column> cols = []
			{
				auto num = [](const Fight&, const Player&, double v) { return Num(v); };
				auto whole = [](const Fight&, const Player&, double v) { return std::to_string(static_cast<long long>(v)); };
				std::vector<Column> v;
				v.push_back({"Healing", "Healing Stats, whole round", [](const Fight&, const Player& p) { return p.HealKnown ? double(p.Heal) : kUnknown; }, num, S_Support});
				v.push_back({"Barrier", "Barrier given, whole round", [](const Fight&, const Player& p) { return p.HealKnown ? double(p.Barrier) : kUnknown; }, num, S_Support});
				v.push_back({"Cleanses", "Ally conditions removed", [](const Fight&, const Player& p) { return double(p.Cleanses); }, whole, S_Support});
				v.push_back({"CC covered", "Subgroup CC hitting their stability", [](const Fight&, const Player& p) { return p.StabAllyMs > 0 && p.StabEligible >= 3 ? double(p.StabCovered) / p.StabEligible : -1.0; },
					[](const Fight&, const Player& p, double v) { return v < 0 ? std::string("-") : Share(p.StabCovered, p.StabEligible, 3); }, S_Support});
				v.push_back({"Reviving s", "Time spent reviving allies", [](const Fight&, const Player& p) { return p.ReviveMs / 1000.0; },
					[](const Fight&, const Player&, double x) { char b[16]; std::snprintf(b, sizeof b, "%.1f", x); return std::string(b); }, S_Support});
				v.push_back({"Revive skills", "Uses with an ally down", [](const Fight&, const Player& p)
					{ int n = 0, all = 0; for (auto& u : p.ReviveUses) { all += u.Done; n += u.Done && u.DownNear > 0; }
					  return all ? double(n) / all : -1.0; },
					[](const Fight&, const Player& p, double v)
					{ if (v < 0) { return std::string("-"); }
					  int n = 0, all = 0; for (auto& u : p.ReviveUses) { all += u.Done; n += u.Done && u.DownNear > 0; }
					  return std::to_string(n) + " of " + std::to_string(all); }, S_Support});
				v.push_back({"Damage", "To enemy players", [](const Fight&, const Player& p) { return double(p.Damage); }, num, S_Damage});
				v.push_back({"Damage all", "To anything hostile", [](const Fight&, const Player& p) { return double(p.DamageAll); }, num, S_Damage});
				v.push_back({"In spikes", "Damage within 2 s of ally spikes", [](const Fight& f, const Player& p)
					{ double in = 0, all = 0;
					  for (size_t s = 0; s < p.DamagePerS.size(); s++)
					  {
						  all += p.DamagePerS[s];
						  if (InSpike(f, true, static_cast<int64_t>(s) * 1000 + 500, 2000, 2000)) { in += p.DamagePerS[s]; }
					  }
					  return all > 0 ? 100.0 * in / all : kUnknown; },
					[](const Fight&, const Player&, double x) { return Pct(x); }, S_Damage});
				v.push_back({"Strips", "Enemy boons removed", [](const Fight&, const Player& p) { return double(p.Strips); }, whole, S_Damage});
				v.push_back({"CC dealt", "CC landed on enemies", [](const Fight&, const Player& p) { return double(p.CcDealt); }, whole, S_Damage});
				v.push_back({"Negated", "Evaded, blocked or absorbed hits", [](const Fight&, const Player& p) { return double(p.Evades + p.Blocks + p.Invulns); }, whole, S_Defence});
				v.push_back({"Taken", "Damage taken", [](const Fight&, const Player& p) { return double(p.DamageTaken); }, num, S_Defence});
				v.push_back({"Pets took", "Enemy hits on their pets", [](const Fight&, const Player& p) { return double(p.PetsTook); }, num, S_Defence});
				v.push_back({"Evades", "Enemy hits evaded", [](const Fight&, const Player& p) { return double(p.Evades); }, whole, S_Defence});
				v.push_back({"Blocks", "Enemy hits blocked", [](const Fight&, const Player& p) { return double(p.Blocks); }, whole, S_Defence});
				v.push_back({"Invulns", "Enemy hits absorbed", [](const Fight&, const Player& p) { return double(p.Invulns); }, whole, S_Defence});
				v.push_back({"CC taken", "Times crowd controlled", [](const Fight&, const Player& p) { return double(p.CcTaken); }, whole, S_Defence});
				// invulnerability used in enemy spikes, of all uses (the user, 2026-10-03): Tale of the August Queen counts for
				// whoever cast it
				v.push_back({"Invuln in spikes", "Distortion used in enemy spikes", [](const Fight&, const Player& p) { return p.DistortionUses ? double(p.DistortionInSpikes) / p.DistortionUses : -1.0; },
					[](const Fight&, const Player& p, double v) { return v < 0 ? std::string("-") : std::to_string(p.DistortionInSpikes) + " of " + std::to_string(p.DistortionUses); }, S_Defence});
				for (int b = 0; b < Analysis::kBoons; b++)
				{
					unsigned sets = S_Boons | (b == 0 ? S_Damage : 0); // Support has CC covered for stability; boons in Boons
					v.push_back({Analysis::kBoonNames[b], "Given to own subgroup", [b](const Fight& f, const Player& p) { return f.GroupGeneration(p, b); },
						[b](const Fight& f, const Player&, double x) { return f.Intensity[b] ? Num(x) : Pct(x); }, sets});
				}
				// one column for both (a set fits the window without a scroll): sorted by downs, then deaths
				v.push_back({"Downed / died", "Times downed, times died", [](const Fight&, const Player& p) { return p.Downs + p.Deaths * 0.01; },
					[](const Fight&, const Player& p, double) { return std::to_string(p.Downs) + " / " + std::to_string(p.Deaths); }, S_Support | S_Damage | S_Defence | S_Boons});
				return v;
			}();
			return cols;
		}

		void UsAndEnemy(const Fight& f)
		{
			if (!ImGui::BeginTable("round", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) { return; }
			const char* heads[] = {"", "Players", "Downed", "Killed", "Damage", "Spikes", "Pets took"};
			for (const char* h : heads) { ImGui::TableSetupColumn(h, ImGuiTableColumnFlags_WidthFixed, h[0] ? 70.0f : 60.0f); }
			Headers({{"", nullptr}, {"Players", nullptr}, {"Downed", "Of this side"}, {"Killed", "Of this side"}, {"Damage", "To the other side's players"},
				{"Spikes", "Damage peaks"}, {"Pets took", "Hits on pets and minions"}});
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); Key(kYou, "Ally");
			NumCell(std::to_string(f.SquadCount)); NumCell(std::to_string(f.SquadDowns)); NumCell(std::to_string(f.SquadDeaths));
			NumCell(Num(double(f.SquadDamage))); NumCell(std::to_string(f.OurSpikesMs.size())); NumCell(Num(double(f.SquadPetsTook)));
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); Key(kEnemy, "Enemy");
			NumCell(std::to_string(f.EnemyCount)); NumCell(std::to_string(f.EnemyDowns)); NumCell(std::to_string(f.EnemyDeaths));
			NumCell(Num(double(f.EnemyDamage))); NumCell(std::to_string(f.TheirSpikesMs.size())); NumCell(Num(double(f.EnemyPetsTook)));
			// two enemy teams at once: a row each (the user, 2026-10-05), players, downed and killed; the rest isn't split
			std::vector<EnemyTeam> teams = EnemyTeams(f);
			if (teams.size() >= 2)
			{
				for (const EnemyTeam& t : teams)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::TextColored(kMuted, "  %s", TeamName(t.Team));
					NumCell(std::to_string(t.Players)); NumCell(std::to_string(t.Downed)); NumCell(std::to_string(t.Killed));
				}
			}
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
					std::to_string(downs(worst)) + " of " + std::to_string(f.SquadDowns) + " ally downs.");
			}
			else { Answer("Too little CC this round to judge stability by subgroup."); }

			std::vector<int> boons = {Analysis::kStability, 7, 2, 4, 10, 3}; // stability, aegis, quickness, protection, resistance, alacrity
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
			// opens on your role's set until you pick one (a damage player landed on Support)
			int set0 = s.ColumnSet >= 0 ? s.ColumnSet : c.MyRole == R_Damage || c.MyRole == R_Strip ? 1 : 0;
			ImGui::TextUnformatted("Players");
			for (int i = 0; i < 6; i++)
			{
				ImGui::SameLine(0, i ? 4.0f : 16.0f);
				if (ImGui::RadioButton(kSetNames[i], set0 == i)) { s.ColumnSet = set0 = i; }
			}
			ImGui::SameLine(0, 20);
			if (set0 == kStabilitySet)
			{
				ImGui::TextColored(kMuted, "Their subgroup over time: Round, Stability over time.");
				StabilityGivers(c);
				return;
			}
			if (set0 == 0 || set0 == 4)
			{
				// healing and barrier are known only for players running Healing Stats
				int known = static_cast<int>(std::count_if(f.Players.begin(), f.Players.end(), [](const Player& p) { return p.HealKnown; }));
				ImGui::NewLine();
				ImGui::TextColored(kMuted, "Healing and barrier come from Healing Stats: %d of %d players ran it, the rest are unknown and listed last.", known, static_cast<int>(f.Players.size()));
			}
			else { ImGui::TextColored(kMuted, "Click a heading to sort, a name to compare with them."); }

			const auto& all = Columns();
			constexpr double kUnknownKey = -1e300;
			std::vector<int> cols;
			unsigned set = set0 == 4 ? ~0u : (1u << set0);
			for (int i = 0; i < static_cast<int>(all.size()); i++) { if (all[i].Sets & set) { cols.push_back(i); } }
			ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
				ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
			float height = std::max(200.0f, ImGui::GetContentRegionAvail().y);
			if (!ImGui::BeginTable("players", 3 + static_cast<int>(cols.size()), flags, ImVec2(0, height))) { return; }
			ImGui::TableSetupScrollFreeze(3, 1);
			ImGui::TableSetupColumn("Sg", ImGuiTableColumnFlags_WidthFixed, 30, 1000);
			// the spec as its icon (the name on hover): the columns fit the window without a scroll
			ImGui::TableSetupColumn("##spec", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() + 4, 1002);
			ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthFixed, 150, 1001);
			for (size_t i = 0; i < cols.size(); i++)
			{
				ImGuiTableColumnFlags cf = ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending | (i == 0 ? ImGuiTableColumnFlags_DefaultSort : 0);
				// as wide as the heading or the numbers need (they were 90 each: a set needed a horizontal scroll)
				float w = std::string(all[cols[i]].Label) == "CC covered" ? 115.0f : std::max(58.0f, ImGui::CalcTextSize(all[cols[i]].Label).x + 22);
				ImGui::TableSetupColumn(all[cols[i]].Label, cf, w, static_cast<ImGuiID>(cols[i]));
			}
			std::vector<std::pair<const char*, const char*>> heads = {{"Sg", "Subgroup"}, {"##spec", "Specialization"}, {"Player", nullptr}};
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
				if (sortId >= 0 && sortId < static_cast<int>(all.size())) { double v = all[sortId].Value(f, p); return std::isnan(v) ? kUnknownKey : v; }
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
				if ((x == kUnknownKey) != (y == kUnknownKey)) { return y == kUnknownKey; } // unknown last, either direction
				return asc ? x < y : x > y;
			});
			double sortedMax = 0;
			if (sortId >= 0 && sortId < static_cast<int>(all.size())) { for (const Player* p : rows) { if (key(*p) != kUnknownKey) { sortedMax = std::max(sortedMax, key(*p)); } } }
			for (const Player* p : rows)
			{
				ImGui::TableNextRow();
				NumCell(std::to_string(p->Subgroup));
				ImGui::TableNextColumn();
				{
					float lh = ImGui::GetTextLineHeight();
					SpecIconAt(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), lh, *p);
					ImGui::Dummy(ImVec2(lh, lh));
					if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", p->Spec.c_str()); }
				}
				ImGui::TableNextColumn();
				std::string name = p->Name + (p->Pov ? " (you)" : "");
				// a name opens Compare: you against them, or you against the best on your spec (the user, 2026-10-05: the way
				// into Compare for anyone, downed or not)
				if (ImGui::Selectable(name.c_str(), false))
				{
					State& st = S();
					Go(P_Compare);
					st.CompareLeft.clear();
					st.CompareRight = p->Pov ? std::string() : p->Account;
					st.Metric = -1;
					st.CompareTonight = false;
				}
				if (ImGui::IsItemHovered()) { ImGui::SetTooltip(p->Pov ? "Click: compare yourself with the best on your spec" : "Click: compare yourself with %s", p->Name.c_str()); }
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
		UsAndEnemy(*c.F);
		ImGui::Spacing();
		Subgroups(c);
		ImGui::Spacing();
		Players(c);
	}
}
