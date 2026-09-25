// Round tab (why did the enemy win): the verdict, three cards of numbers (spikes, stability, allies downed by
// subgroup), skills marked on the round's time line, both sides' damage with their spikes, and every spike in order.
// Our spikes open the spike breakdown (weert's request): the picked skills on one graph around the peak, and every
// damage player's timing. A second view shows a subgroup's stability over time.
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "UiCommon.h"

namespace Ui
{
	namespace
	{
		constexpr int32_t kBin = 250;          // the breakdown's time step
		constexpr int32_t kSpan = 3000;        // and its reach either side of the peak
		constexpr int32_t kOnTime = 1000;      // a damage player's damage centred this close to the peak: on time
		// A colour per picked skill: blue, aqua, yellow, magenta, violet (the dataviz skill's categorical steps for a dark
		// surface, validated on #101114: worst colour-blind separation 8.4, all 3:1+). Orange stays the enemy's.
		const ImU32 kSkillCols[] = {IM_COL32(0x39, 0x87, 0xe5, 255), IM_COL32(0x19, 0x9e, 0x70, 255), IM_COL32(0xc9, 0x85, 0x00, 255),
			IM_COL32(0xd5, 0x51, 0x81, 255), IM_COL32(0x90, 0x85, 0xe9, 255)};
		const ImU32 kWorse = IM_COL32(0xd9, 0x59, 0x26, 255);
		const ImVec4 kWorseV(0xd9 / 255.0f, 0x59 / 255.0f, 0x26 / 255.0f, 1.0f);

		// One damage player in one of our spikes
		struct Lane
		{
			const Player* P = nullptr;
			std::array<double, 2 * kSpan / kBin> Bins{};
			double Damage = 0;
			double Centre = 0;       // ms from the peak, damage-weighted
			std::string Why;         // why not more: down, CC'd, stripped
			int Timing = 0;          // -1 early, 0 on time, 1 late, 2 no damage
		};

		struct Spike
		{
			bool Ours = true;
			int64_t T = 0;            // the detector's peak second (its middle)
			int64_t Peak = 0;         // ours: the peak quarter second
			int Downs = 0;            // ours: enemies downed from 1 s before to 4 s after; theirs: allies downed
			int Died = 0;             // theirs: of those, died
			// ours
			std::vector<Lane> Lanes;  // every damage player, most damage first
			int Alive = 0, OnTime = 0, Late = 0, Early = 0, Down = 0;
			// theirs
			int MinHitters = 0, MaxHitters = 0;
			std::map<int, int> BySubgroup;
			std::string Line;         // what it did, for the list
		};

		struct View
		{
			std::vector<Spike> Spikes; // both sides, in time order
			std::vector<const Player*> DamagePlayers;
			// cards
			int EnemySpikes = 0, OurSpikes = 0, DownsInTheirs = 0, DownsInOurs = 0;
			std::string OnTimeList;
			int DownsNoStab = 0, StabStripped = 0, StabCorrupted = 0, CcWindows = 0, CcCovered = 0;
			int SpikesAfterCmdStripped = 0;
			struct Group { int Sg = 0; int Downed = 0, Died = 0, Windows = 0, Covered = 0; bool Worst = false; };
			std::vector<Group> Groups;
			int CmdGroup = -1;
		};

		bool DownAt(const Player& p, int64_t aMs)
		{
			for (const auto& s : p.DownSpans) { if (s.From <= aMs && s.To >= aMs) { return true; } }
			return false;
		}

		template <typename T> std::string Secs(T aMs, bool aSign = true)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), aSign ? "%+.1f s" : "%.1f s", static_cast<double>(aMs) / 1000.0);
			return buf;
		}

		// Why a damage player did less in a spike, from the 3 s before the peak to 0.5 s after: the first that applies
		std::string WhyNot(const Player& p, int64_t aPeak)
		{
			for (const auto& s : p.DownSpans)
			{
				if (s.From <= aPeak && s.To >= aPeak - 500) { return std::string(s.Dead ? "dead" : "down") + " since " + Duration(s.From); }
			}
			std::string out;
			for (const auto& h : p.CcIn)
			{
				if (h.Ms < aPeak - 3000 || h.Ms > aPeak + 500) { continue; }
				out = std::string(Analysis::kCcVerbs[h.Kind]) + " " + (h.Ms <= aPeak ? Secs(aPeak - h.Ms, false) + " before the peak" : Secs(h.Ms - aPeak, false) + " after the peak");
			}
			for (const auto& st : p.StripsIn)
			{
				if (st.Boon == Analysis::kStability && st.Ms >= aPeak - 3000 && st.Ms <= aPeak)
				{
					out += (out.empty() ? "" : "; ") + std::string("stability ") + (st.Corrupted ? "corrupted" : "stripped") + " at " + Secs(st.Ms - aPeak);
					break;
				}
			}
			return out;
		}

		View Build(const Ctx& c)
		{
			const Fight& f = *c.F;
			View v;
			// Damage players: by their spec's jobs, plus anyone who did at least half the median damage of those (a
			// Chronomancer on a hybrid build who still dealt the second most damage of the squad)
			std::vector<double> dealt;
			for (const Player& p : f.Players) { if (IsDamagePlayer(*c.Fights, p)) { v.DamagePlayers.push_back(&p); dealt.push_back(double(p.Damage)); } }
			if (!dealt.empty())
			{
				std::sort(dealt.begin(), dealt.end());
				double floor = 0.5 * dealt[dealt.size() / 2];
				for (const Player& p : f.Players)
				{
					if (p.Damage >= floor && std::find(v.DamagePlayers.begin(), v.DamagePlayers.end(), &p) == v.DamagePlayers.end()) { v.DamagePlayers.push_back(&p); }
				}
			}
			v.CmdGroup = f.Commander >= 0 ? f.Players[f.Commander].Subgroup : -1;
			auto count = [](const std::vector<int32_t>& aDowns, int64_t aFrom, int64_t aTo)
			{
				return static_cast<int>(std::count_if(aDowns.begin(), aDowns.end(), [&](int32_t d) { return d >= aFrom && d <= aTo; }));
			};
			std::vector<Down> downs = Downs(f);
			std::vector<int> onTime;
			// Our spikes: the peak quarter second, then each damage player's damage around it
			for (int64_t t : f.OurSpikesMs)
			{
				Spike s;
				s.Ours = true;
				s.T = t;
				s.Downs = count(f.EnemyDownMs, t - 1000, t + 4000);
				std::map<int64_t, double> bins;
				for (const Player& p : f.Players) { for (const auto& h : p.HitsOut) { if (h.Ms >= t - 1500 && h.Ms < t + 1500) { bins[h.Ms / kBin] += h.Damage; } } }
				s.Peak = t;
				double best = -1;
				for (auto& [b, d] : bins) { if (d > best) { best = d; s.Peak = b * kBin + kBin / 2; } }
				for (const Player* p : v.DamagePlayers)
				{
					Lane l;
					l.P = p;
					double weighted = 0;
					for (const auto& h : p->HitsOut)
					{
						int64_t d = h.Ms - s.Peak;
						if (d < -kSpan || d >= kSpan) { continue; }
						l.Bins[static_cast<size_t>((d + kSpan) / kBin)] += h.Damage;
						l.Damage += h.Damage;
						weighted += h.Damage * double(d);
					}
					l.Centre = l.Damage > 0 ? weighted / l.Damage : 0;
					l.Why = WhyNot(*p, s.Peak);
					bool down = DownAt(*p, s.Peak);
					l.Timing = l.Damage <= 0 ? 2 : l.Centre > kOnTime ? 1 : l.Centre < -kOnTime ? -1 : 0;
					if (down) { s.Down++; } else { s.Alive++; }
					if (!down) { s.OnTime += l.Timing == 0; s.Late += l.Timing == 1; s.Early += l.Timing == -1; }
					s.Lanes.push_back(l);
				}
				std::sort(s.Lanes.begin(), s.Lanes.end(), [](const Lane& a, const Lane& b) { return a.Damage > b.Damage; });
				s.Line = "downed " + std::to_string(s.Downs) + " \xc2\xb7 " + std::to_string(s.OnTime) + " of " + std::to_string(s.Alive) + " damage players on time";
				if (s.Late) { s.Line += " \xc2\xb7 " + std::to_string(s.Late) + " late"; }
				if (s.Down) { s.Line += " \xc2\xb7 " + std::to_string(s.Down) + " of them down"; }
				v.Spikes.push_back(s);
				v.OurSpikes++;
				onTime.push_back(s.OnTime);
			}
			if (!onTime.empty())
			{
				std::sort(onTime.begin(), onTime.end());
				v.OnTimeList = std::to_string(onTime[onTime.size() / 2]) + " of " + std::to_string(v.DamagePlayers.size());
			}
			// Their spikes: the allies downed, how many enemies hit each, which subgroups
			for (int64_t t : f.TheirSpikesMs)
			{
				Spike s;
				s.Ours = false;
				s.T = s.Peak = t;
				s.MinHitters = 1 << 30;
				for (const Down& d : downs)
				{
					if (d.S.From < t - 1000 || d.S.From > t + 4000) { continue; }
					s.Downs++;
					s.Died += d.Died;
					s.BySubgroup[d.P->Subgroup]++;
					std::set<int> hitters;
					for (const auto& h : d.P->HitsIn) { if (h.Ms >= d.S.From - 6000 && h.Ms <= d.S.From && h.Enemy >= 0) { hitters.insert(h.Enemy); } }
					s.MinHitters = std::min(s.MinHitters, static_cast<int>(hitters.size()));
					s.MaxHitters = std::max(s.MaxHitters, static_cast<int>(hitters.size()));
				}
				if (s.Downs == 0) { s.Line = "downed none of ours"; s.MinHitters = 0; }
				else
				{
					s.Line = "downed " + std::to_string(s.Downs) + (s.Downs == 1 ? " ally" : " allies");
					if (s.Died) { s.Line += " (" + std::to_string(s.Died) + " died)"; }
					if (s.MaxHitters > 0)
					{
						s.Line += s.Downs == 1 ? " \xc2\xb7 hit by " + std::to_string(s.MaxHitters) + " enemies"
							: s.MinHitters == s.MaxHitters ? " \xc2\xb7 each hit by " + std::to_string(s.MaxHitters) + " enemies"
							: " \xc2\xb7 each hit by " + std::to_string(s.MinHitters) + " to " + std::to_string(s.MaxHitters) + " enemies";
					}
					auto most = std::max_element(s.BySubgroup.begin(), s.BySubgroup.end(), [](auto& a, auto& b) { return a.second < b.second; });
					if (s.Downs >= 3 && most->second * 2 > s.Downs) { s.Line += " \xc2\xb7 " + std::to_string(most->second) + " in subgroup " + std::to_string(most->first); }
				}
				// the commander's subgroup lost stability to a strip in the 3 s before it
				if (v.CmdGroup >= 0)
				{
					bool hit = false;
					for (const Player& p : f.Players)
					{
						if (p.Subgroup != v.CmdGroup) { continue; }
						for (const auto& st : p.StripsIn) { hit |= st.Boon == Analysis::kStability && st.Ms >= t - 3000 && st.Ms <= t; }
					}
					v.SpikesAfterCmdStripped += hit;
				}
				v.Spikes.push_back(s);
				v.EnemySpikes++;
			}
			std::sort(v.Spikes.begin(), v.Spikes.end(), [](const Spike& a, const Spike& b) { return a.T < b.T; });
			// Downs counted once even where spike windows overlap
			for (const Down& d : downs)
			{
				bool in = false;
				for (int64_t t : f.TheirSpikesMs) { in |= d.S.From >= t - 1000 && d.S.From <= t + 4000; }
				v.DownsInTheirs += in;
				v.DownsNoStab += !HadBoonAt(*d.P, Analysis::kStability, d.S.From - 200);
			}
			for (int32_t d : f.EnemyDownMs)
			{
				bool in = false;
				for (int64_t t : f.OurSpikesMs) { in |= d >= t - 1000 && d <= t + 4000; }
				v.DownsInOurs += in;
			}
			for (const Player& p : f.Players)
			{
				for (const auto& st : p.StripsIn) { if (st.Boon == Analysis::kStability) { (st.Corrupted ? v.StabCorrupted : v.StabStripped)++; } }
			}
			// Subgroups: allies downed and died, CC covered by anyone's stability
			std::map<int, View::Group> groups;
			for (const Player& p : f.Players) { groups[p.Subgroup].Sg = p.Subgroup; }
			for (const Down& d : downs) { groups[d.P->Subgroup].Downed++; groups[d.P->Subgroup].Died += d.Died; }
			for (auto& [g, n] : f.GroupCcWindows) { groups[g].Windows = n; v.CcWindows += n; }
			for (auto& [g, n] : f.GroupCcCovered) { groups[g].Covered = n; v.CcCovered += n; }
			View::Group* worst = nullptr;
			for (auto& [g, x] : groups)
			{
				v.Groups.push_back(x);
			}
			for (auto& x : v.Groups)
			{
				if (x.Downed == 0) { continue; }
				if (!worst || x.Downed > worst->Downed || (x.Downed == worst->Downed && x.Covered * std::max(1, worst->Windows) < worst->Covered * std::max(1, x.Windows))) { worst = &x; }
			}
			if (worst && v.Groups.size() > 1) { worst->Worst = true; }
			return v;
		}

		// The view is built once per round (the tab draws every frame)
		const View& ViewOf(const Ctx& c)
		{
			static const Fight* round = nullptr;
			static size_t loaded = 0;
			static View view;
			if (round != c.F || loaded != c.Fights->size())
			{
				round = c.F;
				loaded = c.Fights->size();
				view = Build(c);
			}
			return view;
		}

		std::string SkillName(const Fight& f, int32_t aSkill)
		{
			auto it = f.SkillNames.find(aSkill);
			return it == f.SkillNames.end() ? std::to_string(aSkill) : it->second;
		}

		// A card: a title and bullet lines, bad values in orange with a word that says so
		struct Bullet { std::string Label, Value; bool Bad = false; std::string Tip; };
		// The width a card needs: bullet, the longest label, a gap, the longest value, padding
		float CardWidth(const char* aTitle, const std::vector<Bullet>& aLines)
		{
			float label = 0, value = 0;
			for (const Bullet& b : aLines) { label = std::max(label, ImGui::CalcTextSize(b.Label.c_str()).x); value = std::max(value, ImGui::CalcTextSize(b.Value.c_str()).x); }
			const ImGuiStyle& st = ImGui::GetStyle();
			return std::max(ImGui::CalcTextSize(aTitle).x, 10 + label + 16 + value + st.CellPadding.x * 4) + st.WindowPadding.x * 2 + 2;
		}

		// The values sit in a column of their own, right-aligned, so a long label is cut (full text on hover) and never
		// runs into them
		void Card(const char* aId, const char* aTitle, const std::vector<Bullet>& aLines, float aWidth, float aHeight, bool aWrap)
		{
			ImGui::BeginChild(aId, ImVec2(aWidth, aHeight), true, ImGuiWindowFlags_NoScrollbar);
			ImGui::TextUnformatted(aTitle);
			float value = 0;
			for (const Bullet& b : aLines) { value = std::max(value, ImGui::CalcTextSize(b.Value.c_str()).x); }
			if (ImGui::BeginTable(aId, 2, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, value);
				for (const Bullet& b : aLines)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImVec2 bp = ImGui::GetCursorScreenPos();
					ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(bp.x + 3, bp.y + ImGui::GetTextLineHeight() * 0.5f), 2.0f, ImGui::GetColorU32(kMuted));
					ImGui::SetCursorScreenPos(ImVec2(bp.x + 10, bp.y));
					if (aWrap) { ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x); }
					ImGui::TextColored(kMuted, "%s", b.Label.c_str());
					if (aWrap) { ImGui::PopTextWrapPos(); }
					if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s%s%s", b.Label.c_str(), b.Tip.empty() ? "" : ": ", b.Tip.c_str()); }
					NumCell(b.Value, b.Bad ? &kWorseV : nullptr);
				}
				ImGui::EndTable();
			}
			ImGui::EndChild();
		}

		// "+ Add skill": a list of skills with their damage and casts, most damage first; a pick adds it
		void AddSkillMenu(const char* aId, const Fight& f, const std::vector<std::pair<int32_t, std::pair<double, int>>>& aSkills)
		{
			State& s = S();
			if (ImGui::Button((std::string("+ Add skill##") + aId).c_str())) { ImGui::OpenPopup(aId); }
			if (!ImGui::BeginPopup(aId)) { return; }
			static char filter[64] = "";
			ImGui::SetNextItemWidth(260);
			ImGui::InputTextWithHint("##filter", "type to filter", filter, sizeof(filter));
			ImGui::BeginChild("list", ImVec2(360, std::min(320.0f, (aSkills.size() + 1) * (ImGui::GetTextLineHeight() + 6))));
			std::string want = Lower(filter);
			for (auto& [sk, v] : aSkills)
			{
				std::string name = SkillName(f, sk);
				if (!want.empty() && Lower(name).find(want) == std::string::npos) { continue; }
				bool on = std::find(s.Picked.begin(), s.Picked.end(), sk) != s.Picked.end();
				ImGui::PushID(sk);
				ImVec2 p = ImGui::GetCursorScreenPos();
				if (ImGui::Selectable("##sk", on, ImGuiSelectableFlags_DontClosePopups, ImVec2(0, ImGui::GetTextLineHeight() + 2)))
				{
					if (on) { s.Picked.erase(std::find(s.Picked.begin(), s.Picked.end(), sk)); }
					else { s.Picked.push_back(sk); }
				}
				ImDrawList* dl = ImGui::GetWindowDrawList();
				float lh = ImGui::GetTextLineHeight();
				IconAt(dl, p, lh, sk, name);
				dl->AddText(ImVec2(p.x + lh + 6, p.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
				std::string num = (v.first > 0 ? Num(v.first) + " dmg  " : std::string()) + std::to_string(v.second) + (v.second == 1 ? " cast" : " casts");
				dl->AddText(ImVec2(p.x + 350 - ImGui::CalcTextSize(num.c_str()).x, p.y), ImGui::GetColorU32(kMuted), num.c_str());
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::EndPopup();
		}

		// The picked skills as chips (a click removes one), then "+ Add skill"
		void Chips(const char* aId, const Fight& f, const std::vector<std::pair<int32_t, std::pair<double, int>>>& aSkills, const char* aLabel)
		{
			State& s = S();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(aLabel);
			float lh = ImGui::GetTextLineHeight();
			for (size_t i = 0; i < s.Picked.size(); i++)
			{
				int32_t sk = s.Picked[i];
				std::string name = SkillName(f, sk);
				ImGui::SameLine();
				ImGui::PushID(static_cast<int>(sk));
				ImVec2 p = ImGui::GetCursorScreenPos();
				float w = lh + 8 + ImGui::CalcTextSize(name.c_str()).x + 22;
				bool remove = ImGui::InvisibleButton("chip", ImVec2(w, ImGui::GetFrameHeight()));
				if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Remove"); }
				ImDrawList* dl = ImGui::GetWindowDrawList();
				ImU32 col = kSkillCols[i % 5];
				dl->AddRectFilled(p, ImVec2(p.x + w, p.y + ImGui::GetFrameHeight()), IM_COL32(0x24, 0x3d, 0x63, 255));
				dl->AddRect(p, ImVec2(p.x + w, p.y + ImGui::GetFrameHeight()), col);
				float ty = p.y + ImGui::GetStyle().FramePadding.y;
				IconAt(dl, ImVec2(p.x + 3, ty), lh, sk, name);
				dl->AddText(ImVec2(p.x + lh + 8, ty), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
				dl->AddText(ImVec2(p.x + w - 16, ty), ImGui::GetColorU32(kMuted), "x");
				ImGui::PopID();
				if (remove) { s.Picked.erase(s.Picked.begin() + static_cast<long>(i)); break; }
			}
			ImGui::SameLine();
			AddSkillMenu(aId, f, aSkills);
		}

		// Skills our squad cast this round (or in a window), most damage to players first, then the rest by casts
		std::vector<std::pair<int32_t, std::pair<double, int>>> SquadSkills(const Fight& f, int64_t aFrom, int64_t aTo)
		{
			std::map<int32_t, std::pair<double, int>> by;
			for (const Player& p : f.Players)
			{
				for (const auto& h : p.HitsOut) { if (h.Ms >= aFrom && h.Ms <= aTo && h.Skill > 0) { by[h.Skill].first += h.Damage; } }
				for (auto& [sk, r] : p.Skills)
				{
					if (sk <= 0) { continue; }
					for (int32_t ms : r.CastMs) { if (ms >= aFrom && ms <= aTo) { by[sk].second++; } }
				}
			}
			std::vector<std::pair<int32_t, std::pair<double, int>>> out;
			for (auto& [sk, v] : by) { if (v.second > 0 || v.first > 0) { out.push_back({sk, v}); } }
			std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second.first != b.second.first ? a.second.first > b.second.first : a.second.second > b.second.second; });
			return out;
		}

		// Uses of a skill by any of us: its casts in the window. Damage in the window from before a player's first cast in
		// it (a well cast just before the window, a trait or relic proc with no cast logged) is a use too, at its first
		// hit; CastAt keeps the cast before the window, if any (hits 1 s+ apart without a cast are two uses)
		struct Cast { int32_t Ms; const Player* By; bool Hit = false; int32_t CastAt = INT32_MIN; };
		std::vector<Cast> CastsOf(const Fight& f, int32_t aSkill, int64_t aFrom, int64_t aTo)
		{
			std::vector<Cast> out;
			for (const Player& p : f.Players)
			{
				auto it = p.Skills.find(aSkill);
				if (it == p.Skills.end()) { continue; }
				int32_t firstCast = INT32_MAX, before = INT32_MIN;
				for (int32_t ms : it->second.CastMs)
				{
					if (ms >= aFrom && ms <= aTo) { out.push_back({ms, &p}); firstCast = std::min(firstCast, ms); }
					else if (ms < aFrom) { before = std::max(before, ms); }
				}
				int32_t last = INT32_MIN;
				for (const auto& h : p.HitsOut)
				{
					if (h.Skill != aSkill || h.Ms < aFrom || h.Ms > aTo || h.Ms >= firstCast) { continue; }
					if (last == INT32_MIN || h.Ms - last > 1000) { out.push_back({h.Ms, &p, true, last == INT32_MIN ? before : INT32_MIN}); }
					last = h.Ms;
					if (before != INT32_MIN) { break; } // one earlier cast: its damage is one use
				}
			}
			std::sort(out.begin(), out.end(), [](const Cast& a, const Cast& b) { return a.Ms < b.Ms; });
			return out;
		}

		// ---- the round -------------------------------------------------------------------------------------------------

		void Overview(const Ctx& c, const View& v)
		{
			const Fight& f = *c.F;
			State& s = S();
			float avail = ImGui::GetContentRegionAvail().x;
			float lh = ImGui::GetTextLineHeight();
			// The three cards
			{
				std::vector<Bullet> spikes = {
					{"Their spikes \xc2\xb7 our downs", std::to_string(v.EnemySpikes) + " \xc2\xb7 " + std::to_string(v.DownsInTheirs), v.DownsInTheirs > v.DownsInOurs,
						"Enemy spikes, and our downs from 1 s before one to 4 s after"},
					{"Our spikes \xc2\xb7 their downs", std::to_string(v.OurSpikes) + " \xc2\xb7 " + std::to_string(v.DownsInOurs), v.DownsInOurs < v.DownsInTheirs,
						"Our spikes, and enemy downs from 1 s before one to 4 s after"},
				};
				if (!v.OnTimeList.empty()) { spikes.push_back({"Damage players on time", v.OnTimeList, false, "In the middle spike: damage centred within 1 s of its peak"}); }
				std::vector<Bullet> stab = {
					{"Downed with no stability", std::to_string(v.DownsNoStab) + " of " + std::to_string(f.SquadDowns), f.SquadDowns > 0 && v.DownsNoStab * 2 > f.SquadDowns,
						"No stability on them at the down"},
					{"Stability stripped \xc2\xb7 corrupted", std::to_string(v.StabStripped) + " \xc2\xb7 " + std::to_string(v.StabCorrupted), false,
						"Times the enemy took all of someone's stability"},
					{"CC on us covered", std::to_string(v.CcCovered) + " of " + std::to_string(v.CcWindows), v.CcWindows > 0 && v.CcCovered * 4 < v.CcWindows * 3,
						"CC that hit someone with stability"},
				};
				if (v.CmdGroup >= 0 && v.EnemySpikes > 0)
				{
					stab.push_back({"Spikes after cmd sg stripped", std::to_string(v.SpikesAfterCmdStripped) + " of " + std::to_string(v.EnemySpikes),
						v.SpikesAfterCmdStripped * 2 > v.EnemySpikes, "Commander's subgroup lost stability in the 3 s before"});
				}
				// The subgroup table's columns by their widest text, then each card as wide as it needs; the room left over
				// is shared out, and when there is too little, all three shrink (labels are cut, never the numbers)
				int mine = c.MeRaw ? c.MeRaw->Subgroup : -1;
				auto groupName = [&](const View::Group& g) { return std::to_string(g.Sg) + (g.Sg == v.CmdGroup ? " cmd" : "") + (g.Sg == mine ? " you" : ""); };
				auto covered = [](const View::Group& g) { return g.Windows ? std::to_string(g.Covered) + " of " + std::to_string(g.Windows) : std::string("-"); };
				float cols[4] = {ImGui::CalcTextSize("Sg").x, ImGui::CalcTextSize("Downed").x, ImGui::CalcTextSize("Died").x, ImGui::CalcTextSize("CC covered").x};
				for (const auto& g : v.Groups)
				{
					cols[0] = std::max(cols[0], ImGui::CalcTextSize(groupName(g).c_str()).x + lh); // + the triangle on the worst
					cols[3] = std::max(cols[3], ImGui::CalcTextSize(covered(g).c_str()).x);
				}
				const ImGuiStyle& st = ImGui::GetStyle();
				float w1 = CardWidth("Spikes", spikes), w2 = CardWidth("Stability", stab);
				float w3 = std::max(ImGui::CalcTextSize("Allies downed, by subgroup").x, cols[0] + cols[1] + cols[2] + cols[3] + st.CellPadding.x * 8 + 12) + st.WindowPadding.x * 2 + 2;
				// the table keeps its width; the two cards share the rest (their labels are cut first)
				float room = avail - 16;
				w3 = std::min(w3, room * 0.45f);
				float scale = (room - w3) / (w1 + w2);
				w1 *= scale; w2 *= scale;
				// too narrow for the labels on one line: they wrap to two, and the cards grow
				bool wrap = scale < 1.0f;
				auto lines = [&](const std::vector<Bullet>& aLines, float aWidth)
				{
					float value = 0;
					for (const Bullet& x : aLines) { value = std::max(value, ImGui::CalcTextSize(x.Value.c_str()).x); }
					float room = std::max(20.0f, aWidth - st.WindowPadding.x * 2 - value - st.CellPadding.x * 4 - 12);
					float n = 0;
					for (const Bullet& x : aLines) { n += wrap ? std::min(2.0f, std::ceil(ImGui::CalcTextSize(x.Label.c_str()).x / room)) : 1.0f; }
					return n;
				};
				float row = lh + st.CellPadding.y * 2;
				float h = std::max(lines(spikes, w1), lines(stab, w2)) * row + lh * 2 + st.WindowPadding.y * 2;
				h = std::max(h, (v.Groups.size() + 2) * (lh + st.ItemSpacing.y + st.CellPadding.y * 2) + st.WindowPadding.y * 2);
				Card("spikescard", "Spikes", spikes, w1, h, wrap);
				ImGui::SameLine(0, 8);
				Card("stabcard", "Stability", stab, w2, h, wrap);
				ImGui::SameLine(0, 8);
				ImGui::BeginChild("groupscard", ImVec2(w3, h), true, ImGuiWindowFlags_NoScrollbar);
				ImGui::TextUnformatted("Allies downed, by subgroup");
				if (ImGui::BeginTable("groups", 4, ImGuiTableFlags_SizingFixedFit))
				{
					ImGui::TableSetupColumn("Sg", ImGuiTableColumnFlags_WidthFixed, cols[0] + 4);
					ImGui::TableSetupColumn("Downed", ImGuiTableColumnFlags_WidthFixed, cols[1] + 4);
					ImGui::TableSetupColumn("Died", ImGuiTableColumnFlags_WidthFixed, cols[2] + 4);
					ImGui::TableSetupColumn("CC covered", ImGuiTableColumnFlags_WidthFixed, cols[3] + 4);
					Headers({{"Sg", "Subgroup"}, {"Downed", "Allies downed"}, {"Died", nullptr}, {"CC covered", "CC hitting someone with stability"}});
					for (const auto& g : v.Groups)
					{
						ImGui::TableNextRow();
						const ImVec4* col = g.Worst ? &kWorseV : nullptr;
						std::string name = groupName(g);
						Cell(name, col);
						if (g.Worst)
						{
							// the worst: orange, and a down triangle (never colour alone)
							ImVec2 e = ImGui::GetItemRectMax();
							Mark(ImGui::GetWindowDrawList(), ImVec2(e.x + lh * 0.6f, e.y - lh * 0.5f), lh * 0.3f, 4, kWorse);
							if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Most allies downed"); }
						}
						NumCell(std::to_string(g.Downed), col);
						NumCell(std::to_string(g.Died), col);
						NumCell(covered(g), col);
					}
					ImGui::EndTable();
				}
				ImGui::EndChild();
			}

			// Skills marked on the time line
			ImGui::Spacing();
			Chips("roundskills", f, SquadSkills(f, 0, f.DurationMs), "Mark skills on the time line");
			if (!s.Picked.empty()) { ImGui::SameLine(0, 12); ImGui::AlignTextToFramePadding(); ImGui::TextColored(kMuted, "an icon per cast by one of us"); }

			// The time line: marked skills' rails, both sides' damage, the spikes
			const float labelW = 120;
			float width = ImGui::GetContentRegionAvail().x - labelW;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 origin = ImGui::GetCursorScreenPos();
			ImVec2 top(origin.x + labelW, origin.y);
			double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
			auto x = [&](double ms) { return top.x + static_cast<float>(std::clamp(ms / span, 0.0, 1.0)) * width; };
			const float rail = lh + 6, graphH = lh * 10;
			float railsH = rail * s.Picked.size();
			float h = railsH + graphH;
			ImGui::SetCursorScreenPos(top);
			ImGui::InvisibleButton("roundgraph", ImVec2(width, h));
			bool hovered = ImGui::IsItemHovered();
			dl->AddRectFilled(top, ImVec2(top.x + width, top.y + h), kLaneBg);
			// spikes: ours across the whole height with the enemies they downed; theirs in the lower half
			for (const Spike& sp : v.Spikes)
			{
				if (sp.Ours)
				{
					dl->AddRectFilled(ImVec2(x(sp.T - 1500.0), top.y), ImVec2(x(sp.T + 1500.0), top.y + h), kOurBand);
					if (sp.Downs > 0)
					{
						Mark(dl, ImVec2(x(sp.T - 1500.0) + 6, top.y + railsH + lh * 0.5f), lh * 0.3f, 3, kYou);
						SmallText(dl, ImVec2(x(sp.T - 1500.0) + 12, top.y + railsH), ImGui::GetColorU32(ImGuiCol_Text), std::to_string(sp.Downs));
					}
				}
				else
				{
					float mid = top.y + railsH + graphH * 0.5f;
					dl->AddRectFilled(ImVec2(x(double(sp.T)), mid), ImVec2(x(sp.T + 3000.0), top.y + h), kEnemyBand);
					if (sp.Downs > 0)
					{
						Mark(dl, ImVec2(x(double(sp.T)) + 6, top.y + h - lh * 0.5f), lh * 0.3f, 4, kEnemy);
						SmallText(dl, ImVec2(x(double(sp.T)) + 12, top.y + h - lh), ImGui::GetColorU32(ImGuiCol_Text), std::to_string(sp.Downs));
					}
				}
			}
			// rails: an icon per cast; icons that would overlap become a count
			for (size_t i = 0; i < s.Picked.size(); i++)
			{
				int32_t sk = s.Picked[i];
				float ry = top.y + rail * i;
				dl->AddLine(ImVec2(top.x, ry + rail - 1), ImVec2(top.x + width, ry + rail - 1), IM_COL32(0x22, 0x25, 0x2b, 255));
				std::string name = SkillName(f, sk);
				dl->PushClipRect(ImVec2(origin.x, ry), ImVec2(top.x - 4, ry + rail), true);
				SmallText(dl, ImVec2(origin.x, ry + 3), ImGui::GetColorU32(kMuted), name);
				dl->PopClipRect();
				std::vector<Cast> casts = CastsOf(f, sk, 0, f.DurationMs);
				for (size_t a = 0; a < casts.size();)
				{
					size_t b = a;
					while (b < casts.size() && x(casts[b].Ms) - x(casts[a].Ms) < lh) { b++; }
					float cx = x(casts[a].Ms);
					IconAt(dl, ImVec2(cx, ry + 3), lh, sk, name);
					if (b - a > 1)
					{
						std::string n = std::to_string(b - a);
						dl->AddRectFilled(ImVec2(cx + lh - 2, ry + 1), ImVec2(cx + lh + ImGui::CalcTextSize(n.c_str()).x * 0.85f + 2, ry + lh * 0.8f), kYou);
						SmallText(dl, ImVec2(cx + lh, ry), IM_COL32(16, 16, 17, 255), n);
					}
					a = b;
				}
			}
			// both sides' damage per second: ours up, theirs down
			float g0 = top.y + railsH, mid = g0 + graphH * 0.5f;
			{
				ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
				double secs = std::max(1.0, f.DurationMs / 1000.0);
				dl->AddText(ImVec2(origin.x, g0 + 2), ink, "Our damage /s");
				SmallText(dl, ImVec2(origin.x, g0 + lh + 2), muted, Num(double(f.SquadDamage) / secs) + " /s average");
				dl->AddText(ImVec2(origin.x, top.y + h - lh * 2 - 2), ink, "Enemy damage /s");
				SmallText(dl, ImVec2(origin.x, top.y + h - lh - 2), muted, Num(double(f.EnemyDamage) / secs) + " /s average");
			}
			double mx = 1;
			for (int64_t d : f.OutPerS) { mx = std::max(mx, double(d)); }
			for (int64_t d : f.InPerS) { mx = std::max(mx, double(d)); }
			size_t n = std::max(f.OutPerS.size(), f.InPerS.size());
			float bw = std::max(1.0f, width / std::max<size_t>(1, n) * 0.72f);
			for (size_t sec = 0; sec < n; sec++)
			{
				float bx = x(sec * 1000.0);
				if (sec < f.OutPerS.size()) { float bh = static_cast<float>(f.OutPerS[sec] / mx) * graphH * 0.45f; Rect(dl, ImVec2(bx, mid - bh), bw, bh, kYou); }
				if (sec < f.InPerS.size()) { Rect(dl, ImVec2(bx, mid + 1), bw, static_cast<float>(f.InPerS[sec] / mx) * graphH * 0.45f, kEnemy); }
			}
			dl->AddLine(ImVec2(top.x, mid), ImVec2(top.x + width, mid), IM_COL32(58, 62, 69, 255));
			if (hovered)
			{
				ImVec2 m = ImGui::GetIO().MousePos;
				int sec = std::clamp(static_cast<int>((m.x - top.x) / width * span / 1000.0), 0, static_cast<int>(n ? n - 1 : 0));
				dl->AddLine(ImVec2(m.x, top.y), ImVec2(m.x, top.y + h), ImGui::GetColorU32(ImGuiCol_Text));
				ImGui::BeginTooltip();
				ImGui::Text("%s", Duration(sec * 1000LL).c_str());
				int row = m.y < g0 ? static_cast<int>((m.y - top.y) / rail) : -1;
				if (row >= 0 && row < static_cast<int>(s.Picked.size()))
				{
					int32_t at = static_cast<int32_t>((m.x - top.x) / width * span);
					for (const Cast& k : CastsOf(f, s.Picked[row], at - 1500, at + 1500))
					{
						ImGui::Text("%s  %s: %s", Duration(k.Ms).c_str(), k.By->Name.c_str(), SkillName(f, s.Picked[row]).c_str());
					}
				}
				else
				{
					ImGui::Text("%s our damage /s", Num(sec < static_cast<int>(f.OutPerS.size()) ? double(f.OutPerS[sec]) : 0.0).c_str());
					ImGui::Text("%s enemy damage /s", Num(sec < static_cast<int>(f.InPerS.size()) ? double(f.InPerS[sec]) : 0.0).c_str());
					int ours = 0, theirs = 0;
					for (int32_t d : f.SquadDownMs) { ours += d / 1000 == sec; }
					for (int32_t d : f.EnemyDownMs) { theirs += d / 1000 == sec; }
					if (ours || theirs) { ImGui::Text("%d of ours downed, %d of theirs", ours, theirs); }
				}
				ImGui::EndTooltip();
			}
			TimeAxis(f, top.x, width);
			Key(kYou, "our damage /s");
			Key(kEnemy, "enemy damage /s");
			Key(kOurBand, "our spike");
			ShapeKey(3, kYou, "enemies it downed");
			Key(kEnemyBand, "enemy spike");
			ShapeKey(4, kEnemy, "allies it downed");
			ImGui::NewLine();

			// Every spike in order
			ImGui::Spacing();
			ImGui::TextUnformatted("Every spike, in order");
			ImGui::SameLine(0, 12);
			ImGui::TextColored(kMuted, "ours break down by damage player; theirs open the allies they downed");
			if (v.Spikes.empty()) { ImGui::TextColored(kMuted, "No spikes this round."); return; }
			if (ImGui::BeginTable("spikelist", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, 44);
				ImGui::TableSetupColumn("who", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Enemy spike").x + ImGui::GetTextLineHeight() + 12);
				ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("go", ImGuiTableColumnFlags_WidthFixed, 110);
				for (const Spike& sp : v.Spikes)
				{
					ImGui::TableNextRow();
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, sp.Ours ? IM_COL32(0x14, 0x1a, 0x24, 255) : IM_COL32(0x1d, 0x16, 0x12, 255));
					Cell(Duration(sp.T), &kMuted);
					ImGui::TableNextColumn();
					Key(sp.Ours ? kYou : kEnemy, "");
					ImGui::TextUnformatted(sp.Ours ? "Our spike" : "Enemy spike");
					Cell(sp.Line);
					ImGui::TableNextColumn();
					ImGui::PushID(static_cast<int>(sp.T) * 2 + sp.Ours);
					if (sp.Ours && ImGui::SmallButton("Break down >")) { s.SpikeOpen = sp.T; s.SpikeStamp = f.Stamp; }
					if (!sp.Ours && sp.Downs > 0 && ImGui::SmallButton("Allied downs >")) { s.DeathSpike = sp.T; s.DeathKey.clear(); s.DeathFilter = 0; s.SwitchTo = T_Deaths; }
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}

		// ---- one of our spikes, broken down -------------------------------------------------------------------------

		void Breakdown(const Ctx& c, const View& v, const Spike& sp)
		{
			const Fight& f = *c.F;
			State& s = S();
			float lh = ImGui::GetTextLineHeight();
			// Header: back, which spike, stepping through our spikes
			std::vector<const Spike*> ours;
			for (const Spike& x : v.Spikes) { if (x.Ours) { ours.push_back(&x); } }
			size_t at = std::find(ours.begin(), ours.end(), &sp) - ours.begin();
			if (ImGui::Button("< All spikes")) { s.SpikeOpen = -1; return; }
			ImGui::SameLine(0, 12);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Our spike at %s", Duration(sp.T).c_str());
			ImGui::SameLine(0, 10);
			ImGui::TextColored(kMuted, "%s", sp.Line.c_str());
			float stepW = ImGui::CalcTextSize("< Spike").x + ImGui::CalcTextSize("Spike >").x + ImGui::CalcTextSize("00 of 00").x + ImGui::GetStyle().FramePadding.x * 4 + 24;
			ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowContentRegionMax().x - stepW));
			if (ImGui::SmallButton("< Spike") && at > 0) { s.SpikeOpen = ours[at - 1]->T; }
			ImGui::SameLine();
			ImGui::TextColored(kMuted, "%d of %d", static_cast<int>(at + 1), static_cast<int>(ours.size()));
			ImGui::SameLine();
			if (ImGui::SmallButton("Spike >") && at + 1 < ours.size()) { s.SpikeOpen = ours[at + 1]->T; }

			const int64_t from = sp.Peak - kSpan, to = sp.Peak + kSpan;
			Chips("spikeskills", f, SquadSkills(f, from, to), "Skills");

			// The graph: each picked skill's damage per quarter second (all our casts added up) on one axis; shaded
			// behind, all our damage on its own scale
			constexpr int kBins = 2 * kSpan / kBin;
			std::vector<std::array<double, kBins>> lines(s.Picked.size());
			std::array<double, kBins> all{};
			for (const Player& p : f.Players)
			{
				for (const auto& h : p.HitsOut)
				{
					if (h.Ms < from || h.Ms >= to) { continue; }
					size_t b = static_cast<size_t>((h.Ms - from) / kBin);
					all[b] += h.Damage;
					for (size_t i = 0; i < s.Picked.size(); i++) { if (h.Skill == s.Picked[i]) { lines[i][b] += h.Damage; } }
				}
			}
			double mx = 1, allMx = 1;
			for (auto& l : lines) { for (double d : l) { mx = std::max(mx, d); } }
			for (double d : all) { allMx = std::max(allMx, d); }
			if (s.Picked.empty()) { mx = allMx; }
			const float labelW = 200;
			float width = ImGui::GetContentRegionAvail().x;
			float gw = width - labelW - 10;
			const float gh = lh * 12;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 top = ImGui::GetCursorScreenPos();
			ImVec2 g(top.x + labelW, top.y + lh * 1.3f); // room above for the peak labels
			auto x = [&](auto ms) { return g.x + static_cast<float>((static_cast<double>(ms) - static_cast<double>(from)) / double(to - from)) * gw; };
			auto y = [&](double d) { return g.y + gh - static_cast<float>(d / mx) * (gh - 4); };
			ImGui::InvisibleButton("spikegraph", ImVec2(width, gh + lh * 2.5f));
			bool hovered = ImGui::IsItemHovered();
			dl->AddRectFilled(g, ImVec2(g.x + gw, g.y + gh), kLaneBg);
			dl->AddRectFilled(ImVec2(x(sp.Peak - 500.0), g.y), ImVec2(x(sp.Peak + 500.0), g.y + gh), kOurBand);
			// all our damage, shaded, on its own scale
			for (int b = 0; b < kBins; b++)
			{
				float hgt = static_cast<float>(all[b] / allMx) * (gh - 4);
				dl->AddRectFilled(ImVec2(x(from + b * kBin) + 1, g.y + gh - hgt), ImVec2(x(from + (b + 1) * kBin) - 1, g.y + gh), IM_COL32(0x2c, 0x2f, 0x35, 255));
			}
			dl->AddLine(ImVec2(x(double(sp.Peak)), g.y - lh * 1.3f), ImVec2(x(double(sp.Peak)), g.y + gh), ImGui::GetColorU32(ImGuiCol_Text));
			// a step line per skill, its mark and name at the peak
			for (size_t i = 0; i < lines.size(); i++)
			{
				ImU32 col = kSkillCols[i % 5];
				ImVec2 prev(x(double(from)), y(lines[i][0]));
				int peak = 0;
				for (int b = 0; b < kBins; b++)
				{
					float yy = y(lines[i][b]);
					if (b > 0) { dl->AddLine(ImVec2(x(from + b * kBin), prev.y), ImVec2(x(from + b * kBin), yy), col, 2.0f); }
					dl->AddLine(ImVec2(x(from + b * kBin), yy), ImVec2(x(from + (b + 1) * kBin), yy), col, 2.0f);
					prev = ImVec2(x(from + (b + 1) * kBin), yy);
					if (lines[i][b] > lines[i][peak]) { peak = b; }
				}
				std::string label = SkillName(f, s.Picked[i]);
				float lx = std::min(x(from + peak * kBin + kBin / 2) + 4, g.x + gw - ImGui::CalcTextSize(label.c_str()).x - lh);
				float ly = y(lines[i][peak]) - lh - 1;
				Mark(dl, ImVec2(lx + lh * 0.4f, ly + lh * 0.5f), lh * 0.33f, static_cast<int>(i % 5), col);
				dl->AddText(ImVec2(lx + lh, ly), col, label.c_str());
			}
			// the axis
			for (int t = -3; t <= 3; t++)
			{
				std::string label = t == 0 ? "peak" : (t > 0 ? "+" : "") + std::to_string(t) + " s";
				SmallText(dl, ImVec2(x(sp.Peak + t * 1000.0) - ImGui::CalcTextSize(label.c_str()).x * 0.4f, g.y + gh + 2), ImGui::GetColorU32(kMuted), label);
			}
			// the legend at the left
			{
				float ly = g.y;
				ImU32 muted = ImGui::GetColorU32(kMuted);
				if (s.Picked.empty()) { dl->AddText(ImVec2(top.x, ly), muted, "Add skills to see"); dl->AddText(ImVec2(top.x, ly + lh), muted, "their casts here."); ly += lh * 2.5f; }
				for (size_t i = 0; i < s.Picked.size(); i++)
				{
					double sum = 0;
					for (double d : lines[i]) { sum += d; }
					std::vector<Cast> uses = CastsOf(f, s.Picked[i], from, to - 1);
					int casts = static_cast<int>(uses.size());
					bool hits = std::any_of(uses.begin(), uses.end(), [](const Cast& k) { return k.Hit; });
					Mark(dl, ImVec2(top.x + lh * 0.4f, ly + lh * 0.5f), lh * 0.33f, static_cast<int>(i % 5), kSkillCols[i % 5]);
					dl->AddText(ImVec2(top.x + lh, ly), kSkillCols[i % 5], SkillName(f, s.Picked[i]).c_str());
					SmallText(dl, ImVec2(top.x + 12, ly + lh), muted, Num(sum) + " damage, " + std::to_string(casts) + (hits ? (casts == 1 ? " use" : " uses") : (casts == 1 ? " cast" : " casts")));
					ly += lh * 2.3f;
				}
				dl->AddRectFilled(ImVec2(top.x, ly + 3), ImVec2(top.x + 12, ly + lh - 3), IM_COL32(0x2c, 0x2f, 0x35, 255));
				SmallText(dl, ImVec2(top.x + 16, ly), muted, "all our damage (own scale)");
				SmallText(dl, ImVec2(top.x + 16, ly + lh), muted, "blue: 0.5 s either side");
			}
			if (hovered)
			{
				float mxp = ImGui::GetIO().MousePos.x;
				if (mxp >= g.x && mxp < g.x + gw)
				{
					int b = std::clamp(static_cast<int>((mxp - g.x) / gw * kBins), 0, kBins - 1);
					ImGui::BeginTooltip();
					ImGui::Text("%s to %s", Secs(from + b * kBin - sp.Peak).c_str(), Secs(from + (b + 1) * kBin - sp.Peak).c_str());
					ImGui::Text("%s all our damage", Num(all[b]).c_str());
					for (size_t i = 0; i < lines.size(); i++) { ImGui::Text("%s %s", Num(lines[i][b]).c_str(), SkillName(f, s.Picked[i]).c_str()); }
					for (size_t i = 0; i < s.Picked.size(); i++)
					{
						for (const Cast& k : CastsOf(f, s.Picked[i], from + b * kBin - 250, from + (b + 1) * kBin + 250))
						{
							std::string castAt = k.CastAt != INT32_MIN ? " (cast at " + Secs(k.CastAt - sp.Peak) + ")" : std::string();
							ImGui::TextColored(kMuted, "%s %s %s at %s%s", k.By->Name.c_str(), k.Hit ? "hit with" : "cast", SkillName(f, s.Picked[i]).c_str(), Secs(k.Ms - sp.Peak).c_str(), castAt.c_str());
						}
					}
					ImGui::EndTooltip();
				}
			}

			// Who used them: a lane per picked skill on the graph's clock, an icon per use with the player's name beside it
			// (the user's v5: the graph keeps only the lines). A thin frame: the first hit of a use with no cast logged.
			if (!s.Picked.empty())
			{
				ImGui::TextUnformatted("Who used them");
				ImGui::SameLine(0, 12);
				ImGui::TextColored(kMuted, "a lane per skill; thin frame: first hit (cast before this window, or no cast logged)");
				const float rh2 = lh + 8;
				for (size_t i = 0; i < s.Picked.size(); i++)
				{
					ImVec2 p = ImGui::GetCursorScreenPos();
					ImGui::PushID(static_cast<int>(i) + 7000);
					ImGui::InvisibleButton("uses", ImVec2(width, rh2));
					bool over = ImGui::IsItemHovered();
					ImGui::PopID();
					ImU32 col = kSkillCols[i % 5], muted = ImGui::GetColorU32(kMuted);
					std::string name = SkillName(f, s.Picked[i]);
					dl->PushClipRect(p, ImVec2(p.x + labelW - 6, p.y + rh2), true);
					Mark(dl, ImVec2(p.x + lh * 0.4f, p.y + rh2 * 0.5f), lh * 0.33f, static_cast<int>(i % 5), col);
					dl->AddText(ImVec2(p.x + lh, p.y + (rh2 - lh) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
					dl->PopClipRect();
					ImVec2 lp(g.x, p.y);
					dl->AddRectFilled(lp, ImVec2(lp.x + gw, lp.y + rh2 - 2), IM_COL32(0x15, 0x17, 0x1b, 255));
					dl->AddRectFilled(lp, ImVec2(lp.x + 3, lp.y + rh2 - 2), col);
					dl->AddLine(ImVec2(x(double(sp.Peak)), lp.y), ImVec2(x(double(sp.Peak)), lp.y + rh2 - 2), IM_COL32(0x5a, 0x5f, 0x68, 255));
					std::vector<Cast> uses = CastsOf(f, s.Picked[i], from, to - 1);
					for (size_t k = 0; k < uses.size(); k++)
					{
						float ix = x(double(uses[k].Ms)) - lh * 0.5f, iy = lp.y + 3;
						IconAt(dl, ImVec2(ix, iy), lh, s.Picked[i], name);
						dl->AddRect(ImVec2(ix - 1, iy - 1), ImVec2(ix + lh + 1, iy + lh + 1), col, 0, 0, uses[k].Hit ? 1.0f : 2.0f);
						// the name beside it when there is room before the next icon
						float room = (k + 1 < uses.size() ? x(double(uses[k + 1].Ms)) - lh * 0.5f : g.x + gw) - (ix + lh + 3);
						std::string who = uses[k].By->Name;
						if (ImGui::CalcTextSize(who.c_str()).x * 0.85f <= room) { SmallText(dl, ImVec2(ix + lh + 3, iy + 1), muted, who); }
					}
					if (over)
					{
						float mxp = ImGui::GetIO().MousePos.x;
						ImGui::BeginTooltip();
						ImGui::TextUnformatted(name.c_str());
						for (const Cast& k : uses)
						{
							if (std::abs(x(double(k.Ms)) - mxp) > lh * 2) { continue; }
							std::string what = !k.Hit ? std::string() : k.CastAt != INT32_MIN ? "(first hit; cast at " + Secs(k.CastAt - sp.Peak) + ", before this window)" : std::string("(first hit, no cast logged)");
							ImGui::Text("%s  %s %s", Secs(k.Ms - sp.Peak).c_str(), k.By->Name.c_str(), what.c_str());
						}
						ImGui::EndTooltip();
					}
				}
			}

			// Every damage player: their damage around the peak, where it centred, and why not more
			ImGui::Spacing();
			ImGui::Separator();
			const float laneX = labelW, laneW = std::max(200.0f, width - labelW - 360), dmgX = laneX + laneW + 70;
			{
				ImVec2 p = ImGui::GetCursorScreenPos();
				ImU32 muted = ImGui::GetColorU32(kMuted), ink0 = ImGui::GetColorU32(ImGuiCol_Text);
				dl->AddText(p, muted, "Damage player");
				dl->AddText(ImVec2(p.x + laneX, p.y), muted, "their damage around the peak;");
				float tw = ImGui::CalcTextSize("their damage around the peak;").x;
				Mark(dl, ImVec2(p.x + laneX + tw + 10, p.y + lh * 0.5f), lh * 0.33f, 2, ink0);
				dl->AddText(ImVec2(p.x + laneX + tw + 18, p.y), muted, "where it centred");
				std::string d = "Damage";
				dl->AddText(ImVec2(p.x + dmgX - ImGui::CalcTextSize(d.c_str()).x, p.y), muted, d.c_str());
				dl->AddText(ImVec2(p.x + dmgX + 12, p.y), muted, "Timing, why not more");
				ImGui::Dummy(ImVec2(width, lh));
			}
			double laneMx = 1;
			for (const Lane& l : sp.Lanes) { for (double d : l.Bins) { laneMx = std::max(laneMx, d); } }
			for (const Lane& l : sp.Lanes)
			{
				ImVec2 p = ImGui::GetCursorScreenPos();
				float rh = lh + 4;
				ImGui::Dummy(ImVec2(width, rh));
				ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
				// the class icon, then the name (the spec on hover)
				std::string who = l.P->Name + (l.P->Pov ? " (you)" : "");
				SpecIconAt(dl, ImVec2(p.x, p.y + 2), lh, *l.P);
				dl->PushClipRect(p, ImVec2(p.x + laneX - 6, p.y + rh), true);
				dl->AddText(ImVec2(p.x + lh + 5, p.y + 2), ink, who.c_str());
				dl->PopClipRect();
				if (ImGui::IsItemHovered() && ImGui::GetIO().MousePos.x < p.x + laneX) { ImGui::SetTooltip("%s", l.P->Spec.c_str()); }
				ImVec2 lp(p.x + laneX, p.y);
				dl->AddRectFilled(lp, ImVec2(lp.x + laneW, lp.y + rh), kLaneBg);
				float cxPeak = lp.x + laneW * 0.5f;
				dl->AddLine(ImVec2(cxPeak, lp.y), ImVec2(cxPeak, lp.y + rh), IM_COL32(0x5a, 0x5f, 0x68, 255));
				float bwid = laneW / kBins;
				for (int b = 0; b < kBins; b++)
				{
					float hgt = static_cast<float>(l.Bins[b] / laneMx) * (rh - 2);
					if (hgt > 0) { dl->AddRectFilled(ImVec2(lp.x + b * bwid + 1, lp.y + rh - hgt), ImVec2(lp.x + (b + 1) * bwid - 1, lp.y + rh), kYou); }
				}
				if (l.Damage > 0)
				{
					float cx = lp.x + static_cast<float>((l.Centre + kSpan) / (2.0 * kSpan)) * laneW;
					Mark(dl, ImVec2(cx, lp.y + rh * 0.5f), rh * 0.3f, 2, ink);
				}
				std::string dmg = l.Damage > 0 ? Num(l.Damage) : "-";
				dl->AddText(ImVec2(p.x + dmgX - ImGui::CalcTextSize(dmg.c_str()).x, p.y + 2), ink, dmg.c_str());
				std::string timing = l.Timing == 2 ? "no damage" : l.Timing == 1 ? Secs(l.Centre) + " late" : l.Timing == -1 ? Secs(l.Centre) + " early" : "on time";
				dl->AddText(ImVec2(p.x + dmgX + 12, p.y + 2), l.Timing == 1 || l.Timing == 2 ? kWorse : ink, timing.c_str());
				if (!l.Why.empty())
				{
					float wx = p.x + dmgX + 22 + ImGui::CalcTextSize(timing.c_str()).x;
					SmallText(dl, ImVec2(wx, p.y + 3), muted, l.Why);
					if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s: %s", l.P->Name.c_str(), l.Why.c_str()); }
				}
			}
			ImGui::TextColored(kMuted, "On time: damage centred within 1 s of the peak. ArcDPS doesn't record the call, so the clock is the peak.");
		}
	}

	void RoundTab(const Ctx& c)
	{
		const Fight& f = *c.F;
		State& s = S();
		const View& v = ViewOf(c);
		if (s.SpikeOpen >= 0 && s.SpikeStamp == f.Stamp)
		{
			for (const Spike& sp : v.Spikes) { if (sp.Ours && sp.T == s.SpikeOpen) { Breakdown(c, v, sp); return; } }
		}
		s.SpikeOpen = -1;

		// The verdict, and the view
		std::string verdict = f.EnemyDowns > f.SquadDowns ? "Won" : f.EnemyDowns < f.SquadDowns ? "Lost" : "Even";
		verdict += ": we downed " + std::to_string(f.EnemyDowns) + (f.EnemyDowns == 1 ? " enemy" : " enemies") + ", they downed " + std::to_string(f.SquadDowns) +
			(f.SquadDowns == 1 ? " ally" : " allies") + " (" + std::to_string(f.SquadDeaths) + " died).";
		ImGui::SetWindowFontScale(1.15f);
		ImGui::TextUnformatted(verdict.c_str());
		ImGui::SetWindowFontScale(1.0f);
		float segW = ImGui::CalcTextSize("Spikes").x + ImGui::CalcTextSize("Stability over time").x + ImGui::GetStyle().FramePadding.x * 4 + 2;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowContentRegionMax().x - segW));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
		auto seg = [](const char* aLabel, bool aOn)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(aOn ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
			bool clicked = ImGui::SmallButton(aLabel);
			ImGui::PopStyleColor();
			return clicked;
		};
		if (seg("Spikes", s.RoundView == 0)) { s.RoundView = 0; }
		ImGui::SameLine();
		if (seg("Stability over time", s.RoundView == 1)) { s.RoundView = 1; }
		ImGui::PopStyleVar();
		if (s.RoundView == 1) { StabilityTimeLine(c); return; }
		Overview(c, v);
	}
}
