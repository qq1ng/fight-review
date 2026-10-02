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
			std::vector<Down> downs = Downs(f);
			std::vector<int> onTime;
			// Our spikes: the peak quarter second, then each damage player's damage around it
			for (int64_t t : f.OurSpikesMs)
			{
				Spike s;
				s.Ours = true;
				s.T = t;
				s.Downs = static_cast<int>(std::count_if(f.EnemyDownMs.begin(), f.EnemyDownMs.end(), [&](int32_t d) { return SpikeOf(f.OurSpikesMs, d) == t; }));
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
					if (SpikeOf(f.TheirSpikesMs, d.S.From) != t) { continue; }
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
				v.DownsInTheirs += SpikeOf(f.TheirSpikesMs, d.S.From) >= 0;
				v.DownsNoStab += !HadBoonAt(*d.P, Analysis::kStability, d.S.From - 200);
			}
			for (int32_t d : f.EnemyDownMs)
			{
				v.DownsInOurs += SpikeOf(f.OurSpikesMs, d) >= 0;
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

		// The view is built once per round (the tab draws every frame); the Summary keeps its own tally (OurSpikeTally)
		const View& ViewOf(const Ctx& c)
		{
			static const Fight* round = nullptr;
			static uint64_t version = UINT64_MAX; // the rounds' version: a round re-read may reuse the old one's address
			static View view;
			if (round != c.F || version != DataVersion())
			{
				round = c.F;
				version = DataVersion();
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
		// aLink: a button at the title's right that opens more (returns true when clicked)
		bool Card(const char* aId, const char* aTitle, const std::vector<Bullet>& aLines, float aWidth, float aHeight, bool aWrap, const char* aLink = nullptr)
		{
			bool clicked = false;
			ImGui::BeginChild(aId, ImVec2(aWidth, aHeight), true, ImGuiWindowFlags_NoScrollbar);
			ImGui::TextUnformatted(aTitle);
			if (aLink)
			{
				ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(aLink).x - ImGui::GetStyle().FramePadding.x * 2));
				clicked = ImGui::SmallButton(aLink);
			}
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
			return clicked;
		}

		// "+ Add skill": a list of skills with their damage and casts, most damage first; a pick adds it
		void AddSkillMenu(const char* aId, const Fight& f, const std::vector<std::pair<int32_t, std::pair<double, int>>>& aSkills, std::vector<int32_t>& aPicked,
			const char* aButton, const char* aUse)
		{
			if (ImGui::Button((std::string(aButton) + "##" + aId).c_str())) { ImGui::OpenPopup(aId); }
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
				bool on = std::find(aPicked.begin(), aPicked.end(), sk) != aPicked.end();
				ImGui::PushID(sk);
				ImVec2 p = ImGui::GetCursorScreenPos();
				if (ImGui::Selectable("##sk", on, ImGuiSelectableFlags_DontClosePopups, ImVec2(0, ImGui::GetTextLineHeight() + 2)))
				{
					if (on) { aPicked.erase(std::find(aPicked.begin(), aPicked.end(), sk)); }
					else { aPicked.push_back(sk); }
				}
				ImDrawList* dl = ImGui::GetWindowDrawList();
				float lh = ImGui::GetTextLineHeight();
				IconAt(dl, p, lh, sk, name);
				dl->AddText(ImVec2(p.x + lh + 6, p.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
				std::string num = (v.first > 0 ? Num(v.first) + " dmg  " : std::string()) + std::to_string(v.second) + " " + aUse + (v.second == 1 ? "" : "s");
				dl->AddText(ImVec2(p.x + 350 - ImGui::CalcTextSize(num.c_str()).x, p.y), ImGui::GetColorU32(kMuted), num.c_str());
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::EndPopup();
		}

		// The picked skills as chips (a click removes one), then "+ Add skill"
		// aEnemy: their skills (orange frames, "+ Add enemy skill", counted in hits)
		void Chips(const char* aId, const Fight& f, const std::vector<std::pair<int32_t, std::pair<double, int>>>& aSkills, const char* aLabel,
			std::vector<int32_t>& aPicked, bool aEnemy = false)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(aLabel);
			float lh = ImGui::GetTextLineHeight();
			for (size_t i = 0; i < aPicked.size(); i++)
			{
				int32_t sk = aPicked[i];
				std::string name = SkillName(f, sk);
				ImGui::SameLine();
				ImGui::PushID(static_cast<int>(sk));
				ImVec2 p = ImGui::GetCursorScreenPos();
				float w = lh + 8 + ImGui::CalcTextSize(name.c_str()).x + 22;
				bool remove = ImGui::InvisibleButton("chip", ImVec2(w, ImGui::GetFrameHeight()));
				// with the "+ Add skill" list still open, ImGui spends a click outside it on closing the list: the chip then
				// ignored it (the user, 2026-10-01: "clicking to remove doesn't always work")
				if (!remove && ImGui::IsMouseClicked(0) && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
					ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) { remove = true; }
				if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Remove"); }
				ImDrawList* dl = ImGui::GetWindowDrawList();
				ImU32 col = aEnemy ? kEnemy : kSkillCols[i % 5];
				dl->AddRectFilled(p, ImVec2(p.x + w, p.y + ImGui::GetFrameHeight()), aEnemy ? IM_COL32(0x2a, 0x1c, 0x16, 255) : IM_COL32(0x24, 0x3d, 0x63, 255));
				dl->AddRect(p, ImVec2(p.x + w, p.y + ImGui::GetFrameHeight()), col);
				float ty = p.y + ImGui::GetStyle().FramePadding.y;
				IconAt(dl, ImVec2(p.x + 3, ty), lh, sk, name);
				dl->AddText(ImVec2(p.x + lh + 8, ty), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
				dl->AddText(ImVec2(p.x + w - 16, ty), ImGui::GetColorU32(kMuted), "x");
				ImGui::PopID();
				if (remove) { aPicked.erase(aPicked.begin() + static_cast<long>(i)); break; }
			}
			ImGui::SameLine();
			AddSkillMenu(aId, f, aSkills, aPicked, aEnemy ? "+ Add enemy skill" : "+ Add skill", aEnemy ? "hit" : "cast");
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

		// Enemy skills that hit us (or in a window), most damage first, with their hits
		std::vector<std::pair<int32_t, std::pair<double, int>>> EnemySkills(const Fight& f, int64_t aFrom, int64_t aTo)
		{
			std::map<int32_t, std::pair<double, int>> by;
			for (const Player& p : f.Players)
			{
				for (const auto& h : p.HitsIn) { if (h.Ms >= aFrom && h.Ms <= aTo && h.Skill > 0) { by[h.Skill].first += h.Damage; by[h.Skill].second++; } }
			}
			std::vector<std::pair<int32_t, std::pair<double, int>>> out(by.begin(), by.end());
			std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
			return out;
		}

		// An enemy skill's uses on the round's clock: its hits on any of us, a use per run of hits under 1 s apart (enemy
		// casts aren't in our logs; their hits are)
		struct EnemyUse { int32_t Ms = 0; double Damage = 0; std::set<int> Enemies; std::set<const Player*> Hit; };
		std::vector<EnemyUse> EnemyUses(const Fight& f, int32_t aSkill, int64_t aFrom, int64_t aTo)
		{
			std::vector<std::pair<int32_t, std::pair<const Player*, const Player::TakenHit*>>> hits;
			for (const Player& p : f.Players) { for (const auto& h : p.HitsIn) { if (h.Skill == aSkill && h.Ms >= aFrom && h.Ms <= aTo) { hits.push_back({h.Ms, {&p, &h}}); } } }
			std::sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
			std::vector<EnemyUse> out;
			int32_t last = INT32_MIN;
			for (auto& [ms, ph] : hits)
			{
				if (out.empty() || ms - last > 1000) { out.push_back({}); out.back().Ms = ms; }
				last = ms;
				out.back().Damage += ph.second->Damage;
				if (ph.second->Enemy >= 0) { out.back().Enemies.insert(ph.second->Enemy); }
				out.back().Hit.insert(ph.first);
			}
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
				if (Card("stabcard", "Stability", stab, w2, h, wrap, "Over time >")) { S().RoundView = 1; }
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
			Chips("roundskills", f, SquadSkills(f, 0, f.DurationMs), "Mark skills on the time line", s.Picked);
			if (!s.Picked.empty()) { ImGui::SameLine(0, 12); ImGui::AlignTextToFramePadding(); ImGui::TextColored(kMuted, "an icon per cast by one of us"); }

			// The time line: marked skills' rails, both sides' damage, the spikes
			const float labelW = 120;
			float width = ImGui::GetContentRegionAvail().x - labelW;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 origin = ImGui::GetCursorScreenPos();
			ImVec2 top(origin.x + labelW, origin.y);
			double span = static_cast<double>(std::max<int64_t>(1, f.DurationMs));
			auto x = [&](double ms) { return top.x + static_cast<float>(std::clamp(ms / span, 0.0, 1.0)) * width; };
			// Rails: our marked skills above the graph, theirs below it (their picker sits under their rails, so adding
			// our skills never pushes theirs away; the user, 2026-10-01)
			const float rail = lh + 6, graphH = lh * 10;
			float railsH = rail * s.Picked.size(), enemyH = rail * s.PickedEnemy.size();
			float h = railsH + graphH + enemyH;
			ImGui::SetCursorScreenPos(top);
			ImGui::InvisibleButton("roundgraph", ImVec2(width, h));
			bool hovered = ImGui::IsItemHovered();
			float g0 = top.y + railsH, g1 = g0 + graphH, mid = g0 + graphH * 0.5f;
			dl->AddRectFilled(top, ImVec2(top.x + width, top.y + h), kLaneBg);
			// spikes: ours over our rails and the graph, with the enemies they downed; theirs over the lower half and their rails
			for (const Spike& sp : v.Spikes)
			{
				if (sp.Ours)
				{
					dl->AddRectFilled(ImVec2(x(sp.T - 1500.0), top.y), ImVec2(x(sp.T + 1500.0), g1), kOurBand);
					if (sp.Downs > 0)
					{
						Mark(dl, ImVec2(x(sp.T - 1500.0) + 6, g0 + lh * 0.5f), lh * 0.3f, 3, kYou);
						SmallText(dl, ImVec2(x(sp.T - 1500.0) + 12, g0), ImGui::GetColorU32(ImGuiCol_Text), std::to_string(sp.Downs));
					}
				}
				else
				{
					dl->AddRect(ImVec2(x(double(sp.T - kSpikeBeforeMs)), mid), ImVec2(x(double(sp.T)), top.y + h), kEnemyBand); // build-up: its downs count
					dl->AddRectFilled(ImVec2(x(double(sp.T)), mid), ImVec2(x(sp.T + 3000.0), top.y + h), kEnemyBand);
					if (sp.Downs > 0)
					{
						Mark(dl, ImVec2(x(double(sp.T)) + 6, g1 - lh * 0.5f), lh * 0.3f, 4, kEnemy);
						SmallText(dl, ImVec2(x(double(sp.T)) + 12, g1 - lh), ImGui::GetColorU32(ImGuiCol_Text), std::to_string(sp.Downs));
					}
				}
			}
			// a rail: the skill's name at the left, an icon per use; icons that would overlap become a count
			auto drawRail = [&](float ry, int32_t sk, const std::vector<int32_t>& aMs, ImU32 aFrame, ImU32 aBadge)
			{
				dl->AddLine(ImVec2(top.x, ry), ImVec2(top.x + width, ry), IM_COL32(0x22, 0x25, 0x2b, 255));
				std::string name = SkillName(f, sk);
				dl->PushClipRect(ImVec2(origin.x, ry), ImVec2(top.x - 4, ry + rail), true);
				SmallText(dl, ImVec2(origin.x, ry + 3), ImGui::GetColorU32(kMuted), name);
				dl->PopClipRect();
				for (size_t a = 0; a < aMs.size();)
				{
					size_t b = a;
					while (b < aMs.size() && x(aMs[b]) - x(aMs[a]) < lh) { b++; }
					float cx = x(aMs[a]);
					IconAt(dl, ImVec2(cx, ry + 3), lh, sk, name, aFrame);
					if (b - a > 1)
					{
						std::string cnt = std::to_string(b - a);
						dl->AddRectFilled(ImVec2(cx + lh - 2, ry + 1), ImVec2(cx + lh + ImGui::CalcTextSize(cnt.c_str()).x * 0.85f + 2, ry + lh * 0.8f), aBadge);
						SmallText(dl, ImVec2(cx + lh, ry), IM_COL32(16, 16, 17, 255), cnt);
					}
					a = b;
				}
			};
			for (size_t i = 0; i < s.Picked.size(); i++)
			{
				std::vector<int32_t> ms;
				for (const Cast& k : CastsOf(f, s.Picked[i], 0, f.DurationMs)) { ms.push_back(k.Ms); }
				drawRail(top.y + rail * i, s.Picked[i], ms, 0, kYou);
			}
			for (size_t i = 0; i < s.PickedEnemy.size(); i++)
			{
				std::vector<int32_t> ms;
				for (const EnemyUse& u : EnemyUses(f, s.PickedEnemy[i], 0, f.DurationMs)) { ms.push_back(u.Ms); }
				drawRail(g1 + rail * i, s.PickedEnemy[i], ms, kEnemy, kEnemy);
			}
			// both sides' damage to players per second: ours up, theirs down (ours to enemy players only, as the spikes and
			// their breakdown count it; it counted NPCs and siege too, so the breakdown's numbers looked low)
			const std::vector<int64_t>& out = f.ToPlayersPerS;
			{
				ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);
				double secs = std::max(1.0, f.DurationMs / 1000.0);
				dl->AddText(ImVec2(origin.x, g0 + 2), ink, "Our damage /s");
				double ours = 0;
				for (int64_t d : out) { ours += double(d); }
				SmallText(dl, ImVec2(origin.x, g0 + lh + 2), muted, Num(ours / secs) + " /s average");
				dl->AddText(ImVec2(origin.x, g1 - lh * 2 - 2), ink, "Enemy damage /s");
				SmallText(dl, ImVec2(origin.x, g1 - lh - 2), muted, Num(double(f.EnemyDamage) / secs) + " /s average");
			}
			double mx = 1;
			for (int64_t d : out) { mx = std::max(mx, double(d)); }
			for (int64_t d : f.InPerS) { mx = std::max(mx, double(d)); }
			size_t n = std::max(out.size(), f.InPerS.size());
			float bw = std::max(1.0f, width / std::max<size_t>(1, n) * 0.72f);
			for (size_t sec = 0; sec < n; sec++)
			{
				float bx = x(sec * 1000.0);
				if (sec < out.size()) { float bh = static_cast<float>(out[sec] / mx) * graphH * 0.45f; Rect(dl, ImVec2(bx, mid - bh), bw, bh, kYou); }
				if (sec < f.InPerS.size()) { Rect(dl, ImVec2(bx, mid + 1), bw, static_cast<float>(f.InPerS[sec] / mx) * graphH * 0.45f, kEnemy); }
			}
			dl->AddLine(ImVec2(top.x, mid), ImVec2(top.x + width, mid), IM_COL32(58, 62, 69, 255));
			if (hovered)
			{
				ImVec2 m = ImGui::GetIO().MousePos;
				int sec = std::clamp(static_cast<int>((m.x - top.x) / width * span / 1000.0), 0, static_cast<int>(n ? n - 1 : 0));
				int32_t at = static_cast<int32_t>((m.x - top.x) / width * span);
				dl->AddLine(ImVec2(m.x, top.y), ImVec2(m.x, top.y + h), ImGui::GetColorU32(ImGuiCol_Text));
				ImGui::BeginTooltip();
				ImGui::Text("%s", Duration(sec * 1000LL).c_str());
				int row = m.y < g0 ? static_cast<int>((m.y - top.y) / rail) : -1;
				int enemyRow = m.y >= g1 ? static_cast<int>((m.y - g1) / rail) : -1;
				if (row >= 0 && row < static_cast<int>(s.Picked.size()))
				{
					for (const Cast& k : CastsOf(f, s.Picked[row], at - 1500, at + 1500))
					{
						ImGui::Text("%s  %s: %s", Duration(k.Ms).c_str(), k.By->Name.c_str(), SkillName(f, s.Picked[row]).c_str());
					}
				}
				else if (enemyRow >= 0 && enemyRow < static_cast<int>(s.PickedEnemy.size()))
				{
					for (const EnemyUse& u : EnemyUses(f, s.PickedEnemy[enemyRow], at - 1500, at + 1500))
					{
						std::map<std::string, int> count;
						for (int e : u.Enemies) { count[f.Enemies[e].Spec]++; }
						std::string specs;
						for (auto& [sp, k] : count) { specs += (specs.empty() ? "" : ", ") + (k > 1 ? std::to_string(k) + " " : std::string()) + sp; }
						ImGui::Text("%s  %s on %d of ours, by %s", Duration(u.Ms).c_str(), Num(u.Damage).c_str(), static_cast<int>(u.Hit.size()),
							specs.empty() ? "NPCs or siege" : specs.c_str());
					}
				}
				else
				{
					ImGui::Text("%s our damage /s (to players)", Num(sec < static_cast<int>(f.ToPlayersPerS.size()) ? double(f.ToPlayersPerS[sec]) : 0.0).c_str());
					ImGui::Text("%s enemy damage /s", Num(sec < static_cast<int>(f.InPerS.size()) ? double(f.InPerS[sec]) : 0.0).c_str());
					int ours = 0, theirs = 0;
					for (int32_t d : f.SquadDownMs) { ours += d / 1000 == sec; }
					for (int32_t d : f.EnemyDownMs) { theirs += d / 1000 == sec; }
					if (ours || theirs) { ImGui::Text("%d of ours downed, %d of theirs", ours, theirs); }
				}
				ImGui::EndTooltip();
			}
			TimeAxis(f, top.x, width);
			// their skills' picker, under their rails
			Chips("enemyskills", f, EnemySkills(f, 0, f.DurationMs), "Mark enemy skills", s.PickedEnemy, true);
			if (!s.PickedEnemy.empty()) { ImGui::SameLine(0, 12); ImGui::AlignTextToFramePadding(); ImGui::TextColored(kMuted, "an icon per run of their hits on us"); }
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
			ImGui::TextColored(kMuted, "any spike breaks down; an enemy spike's downs open in Deaths");
			if (v.Spikes.empty()) { ImGui::TextColored(kMuted, "No spikes this round."); return; }
			if (ImGui::BeginTable("spikelist", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("t", ImGuiTableColumnFlags_WidthFixed, 44);
				ImGui::TableSetupColumn("who", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Enemy spike").x + ImGui::GetTextLineHeight() + 12);
				ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("go", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Allied downs >  Break down >").x + 24);
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
					if (!sp.Ours && sp.Downs > 0)
					{
						if (ImGui::SmallButton("Allied downs >")) { s.DeathSpike = sp.T; s.DeathKey.clear(); s.DeathFilter = 0; s.SwitchTo = T_Deaths; }
						ImGui::SameLine();
					}
					if (ImGui::SmallButton("Break down >")) { s.SpikeOpen = sp.T; s.SpikeEnemy = !sp.Ours; s.SpikeStamp = f.Stamp; }
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}

		// A spike's graph, the same for both sides (the user, 2026-10-01: an enemy skill picked in their spike showed on
		// the round's clock, not on the spike's own graph as ours do): the picked skills as chips, all that side's damage
		// as a strip, the picked skills' damage per quarter second below it (one scale each), and who used them
		void SpikeGraph(const Ctx& c, const Spike& sp, bool aEnemy)
		{
			const Fight& f = *c.F;
			State& s = S();
			std::vector<int32_t>& picked = aEnemy ? s.PickedEnemy : s.Picked;
			float lh = ImGui::GetTextLineHeight();
			const int64_t peak = aEnemy ? sp.T : sp.Peak;
			const int64_t from = peak - kSpan, to = peak + kSpan;
			if (aEnemy) { Chips("spikeenemyskills", f, EnemySkills(f, from, to), "Enemy skills", picked, true); }
			else { Chips("spikeskills", f, SquadSkills(f, from, to), "Skills", picked); }

			constexpr int kBins = 2 * kSpan / kBin;
			// a 1/4 s bin's damage as a rate, to read like the round's graph (per second): 56k in a 1/4 s is 225k /s
			constexpr double kPerS = 1000.0 / kBin;
			std::vector<std::array<double, kBins>> lines(picked.size());
			std::array<double, kBins> all{};
			auto add = [&](int32_t aMs, int32_t aSkill, double aDamage)
			{
				if (aMs < from || aMs >= to) { return; }
				size_t b = static_cast<size_t>((aMs - from) / kBin);
				all[b] += aDamage;
				for (size_t i = 0; i < picked.size(); i++) { if (aSkill == picked[i]) { lines[i][b] += aDamage; } }
			};
			for (const Player& p : f.Players)
			{
				for (const auto& h : aEnemy ? p.HitsIn : p.HitsOut) { add(h.Ms, h.Skill, h.Damage); }
			}
			// uses of a picked skill in a window: our casts, or runs of their hits on us
			struct Use { int32_t Ms; std::string Who, Note; bool Thin = false; };
			auto usesOf = [&](int32_t aSkill, int64_t aFrom, int64_t aTo)
			{
				std::vector<Use> out;
				if (aEnemy)
				{
					for (const EnemyUse& u : EnemyUses(f, aSkill, aFrom, aTo))
					{
						std::map<std::string, int> count;
						for (int e : u.Enemies) { count[f.Enemies[e].Spec]++; }
						std::string specs;
						for (auto& [spec, k] : count) { specs += (specs.empty() ? "" : ", ") + (k > 1 ? std::to_string(k) + " " : std::string()) + spec; }
						out.push_back({u.Ms, specs.empty() ? "NPC or siege" : specs, Num(u.Damage) + " on " + std::to_string(u.Hit.size()) + " of ours", true});
					}
				}
				else
				{
					for (const Cast& k : CastsOf(f, aSkill, aFrom, aTo))
					{
						std::string what = !k.Hit ? std::string("cast") : k.CastAt != INT32_MIN ? "first hit; cast at " + Secs(k.CastAt - peak) + ", before this window" : std::string("first hit, no cast logged");
						out.push_back({k.Ms, k.By->Name, what, k.Hit});
					}
				}
				return out;
			};
			double mx = 1, allMx = 1;
			for (auto& l : lines) { for (double d : l) { mx = std::max(mx, d); } }
			for (double d : all) { allMx = std::max(allMx, d); }
			if (picked.empty()) { mx = allMx; }
			const float labelW = 200;
			float width = ImGui::GetContentRegionAvail().x;
			float gw = width - labelW - 10;
			// two plots on one clock, each with one scale (the user's v6 review: one plot with two scales misled): all
			// the side's damage as a strip on top, the picked skills' damage below
			const float stripH = lh * 3, gapH = 6;
			const float gh = lh * 9;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 top = ImGui::GetCursorScreenPos();
			ImVec2 st(top.x + labelW, top.y + 2);            // the strip
			ImVec2 g(top.x + labelW, st.y + stripH + gapH);  // the skills' plot
			auto x = [&](auto ms) { return g.x + static_cast<float>((static_cast<double>(ms) - static_cast<double>(from)) / double(to - from)) * gw; };
			auto y = [&](double d) { return g.y + gh - static_cast<float>(d / mx) * (gh - 4); };
			ImGui::InvisibleButton(aEnemy ? "enemyspikegraph" : "spikegraph", ImVec2(width, g.y - top.y + gh + lh * 1.5f));
			bool hovered = ImGui::IsItemHovered();
			ImU32 mutedCol = ImGui::GetColorU32(kMuted);
			for (ImVec2 o : {st, g})
			{
				float oh = o.y == st.y ? stripH : gh;
				dl->AddRectFilled(o, ImVec2(o.x + gw, o.y + oh), kLaneBg);
				dl->AddRectFilled(ImVec2(x(peak - 500.0), o.y), ImVec2(x(peak + 500.0), o.y + oh), aEnemy ? kEnemyBand : kOurBand);
			}
			for (int b = 0; b < kBins; b++)
			{
				float hgt = static_cast<float>(all[b] / allMx) * (stripH - 3);
				dl->AddRectFilled(ImVec2(x(from + b * kBin) + 1, st.y + stripH - hgt), ImVec2(x(from + (b + 1) * kBin) - 1, st.y + stripH), aEnemy ? kEnemy : IM_COL32(0x5a, 0x5f, 0x68, 255));
			}
			SmallText(dl, ImVec2(st.x + 4, st.y + 1), mutedCol, (aEnemy ? "all their damage to us, peak " : "all our damage to players, peak ") + Num(allMx * kPerS) + " /s");
			if (picked.empty())
			{
				SmallText(dl, ImVec2(g.x + 8, g.y + gh * 0.5f - lh * 0.5f), mutedCol, aEnemy ? "Add enemy skills above, or click one in What hit us below, to see their damage here."
					: "Add skills above, or click one in What hit them below, to see their damage here.");
			}
			// the peak: a bright line on a dark edge through both, and every lane below (the user: the grey got lost)
			dl->AddLine(ImVec2(x(double(peak)), st.y), ImVec2(x(double(peak)), g.y + gh), IM_COL32(0x10, 0x11, 0x14, 255), 5.0f);
			dl->AddLine(ImVec2(x(double(peak)), st.y), ImVec2(x(double(peak)), g.y + gh), ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
			// a step line per skill with its mark at its highest point; the names are in the legend at the left, clear of
			// the lines
			for (size_t i = 0; i < lines.size(); i++)
			{
				ImU32 col = kSkillCols[i % 5];
				ImVec2 prev(x(double(from)), y(lines[i][0]));
				int high = 0;
				for (int b = 0; b < kBins; b++)
				{
					float yy = y(lines[i][b]);
					if (b > 0) { dl->AddLine(ImVec2(x(from + b * kBin), prev.y), ImVec2(x(from + b * kBin), yy), col, 2.0f); }
					dl->AddLine(ImVec2(x(from + b * kBin), yy), ImVec2(x(from + (b + 1) * kBin), yy), col, 2.0f);
					prev = ImVec2(x(from + (b + 1) * kBin), yy);
					if (lines[i][b] > lines[i][high]) { high = b; }
				}
				if (lines[i][high] > 0) { Mark(dl, ImVec2(x(from + high * kBin + kBin / 2), y(lines[i][high]) - lh * 0.45f), lh * 0.33f, static_cast<int>(i % 5), col); }
			}
			if (!lines.empty()) { SmallText(dl, ImVec2(g.x + 4, g.y + 1), mutedCol, "the skills' damage, peak " + Num(mx * kPerS) + " /s"); }
			// the axis
			for (int t = -3; t <= 3; t++)
			{
				std::string label = t == 0 ? "peak" : (t > 0 ? "+" : "") + std::to_string(t) + " s";
				SmallText(dl, ImVec2(x(peak + t * 1000.0) - ImGui::CalcTextSize(label.c_str()).x * 0.4f, g.y + gh + 2), mutedCol, label);
			}
			// the legend at the left
			{
				ImU32 muted = ImGui::GetColorU32(kMuted);
				dl->AddText(ImVec2(top.x, st.y), ImGui::GetColorU32(ImGuiCol_Text), aEnemy ? "All their damage" : "All our damage");
				SmallText(dl, ImVec2(top.x, st.y + lh), muted, "shaded: 0.5 s either side");
				float ly = g.y;
				for (size_t i = 0; i < picked.size(); i++)
				{
					double sum = 0;
					for (double d : lines[i]) { sum += d; }
					std::vector<Use> uses = usesOf(picked[i], from, to - 1);
					int n = static_cast<int>(uses.size());
					bool thin = std::any_of(uses.begin(), uses.end(), [](const Use& u) { return u.Thin; });
					Mark(dl, ImVec2(top.x + lh * 0.4f, ly + lh * 0.5f), lh * 0.33f, static_cast<int>(i % 5), kSkillCols[i % 5]);
					dl->AddText(ImVec2(top.x + lh, ly), kSkillCols[i % 5], SkillName(f, picked[i]).c_str());
					SmallText(dl, ImVec2(top.x + 12, ly + lh), muted, Num(sum) + " damage, " + std::to_string(n) + (thin ? (n == 1 ? " use" : " uses") : (n == 1 ? " cast" : " casts")));
					ly += lh * 2.3f;
				}
			}
			if (hovered)
			{
				float mxp = ImGui::GetIO().MousePos.x;
				if (mxp >= g.x && mxp < g.x + gw)
				{
					int b = std::clamp(static_cast<int>((mxp - g.x) / gw * kBins), 0, kBins - 1);
					ImGui::BeginTooltip();
					ImGui::Text("%s to %s", Secs(from + b * kBin - peak).c_str(), Secs(from + (b + 1) * kBin - peak).c_str());
					ImGui::Text("%s /s %s", Num(all[b] * kPerS).c_str(), aEnemy ? "all their damage to us" : "all our damage to players");
					for (size_t i = 0; i < lines.size(); i++) { ImGui::Text("%s /s %s", Num(lines[i][b] * kPerS).c_str(), SkillName(f, picked[i]).c_str()); }
					for (size_t i = 0; i < picked.size(); i++)
					{
						for (const Use& u : usesOf(picked[i], from + b * kBin - 250, from + (b + 1) * kBin + 250))
						{
							ImGui::TextColored(kMuted, "%s %s at %s (%s)", SkillName(f, picked[i]).c_str(), u.Who.c_str(), Secs(u.Ms - peak).c_str(), u.Note.c_str());
						}
					}
					ImGui::EndTooltip();
				}
			}

			// Who used them: a lane per picked skill on the graph's clock, an icon per use with who beside it (the user's
			// v5: the graph keeps only the lines). A thin frame: a use seen by its first hit (no cast logged; theirs always).
			if (!picked.empty())
			{
				ImGui::TextUnformatted("Who used them");
				const float rh2 = lh + 8;
				for (size_t i = 0; i < picked.size(); i++)
				{
					ImVec2 p = ImGui::GetCursorScreenPos();
					ImGui::PushID(static_cast<int>(i) + (aEnemy ? 8000 : 7000));
					ImGui::InvisibleButton("uses", ImVec2(width, rh2));
					bool over = ImGui::IsItemHovered();
					ImGui::PopID();
					ImU32 col = kSkillCols[i % 5], muted = ImGui::GetColorU32(kMuted);
					std::string name = SkillName(f, picked[i]);
					dl->PushClipRect(p, ImVec2(p.x + labelW - 6, p.y + rh2), true);
					Mark(dl, ImVec2(p.x + lh * 0.4f, p.y + rh2 * 0.5f), lh * 0.33f, static_cast<int>(i % 5), col);
					dl->AddText(ImVec2(p.x + lh, p.y + (rh2 - lh) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
					dl->PopClipRect();
					ImVec2 lp(g.x, p.y);
					dl->AddRectFilled(lp, ImVec2(lp.x + gw, lp.y + rh2 - 2), IM_COL32(0x15, 0x17, 0x1b, 255));
					dl->AddRectFilled(lp, ImVec2(lp.x + 3, lp.y + rh2 - 2), col);
					dl->AddLine(ImVec2(x(double(peak)), lp.y), ImVec2(x(double(peak)), lp.y + rh2 - 2), ImGui::GetColorU32(ImGuiCol_Text), 1.5f);
					std::vector<Use> uses = usesOf(picked[i], from, to - 1);
					for (size_t k = 0; k < uses.size(); k++)
					{
						float ix = x(double(uses[k].Ms)) - lh * 0.5f, iy = lp.y + 3;
						IconAt(dl, ImVec2(ix, iy), lh, picked[i], name);
						dl->AddRect(ImVec2(ix - 1, iy - 1), ImVec2(ix + lh + 1, iy + lh + 1), col, 0, 0, uses[k].Thin ? 1.0f : 2.0f);
						// who beside it when there is room before the next icon
						float room = (k + 1 < uses.size() ? x(double(uses[k + 1].Ms)) - lh * 0.5f : g.x + gw) - (ix + lh + 3);
						if (ImGui::CalcTextSize(uses[k].Who.c_str()).x * 0.85f <= room) { SmallText(dl, ImVec2(ix + lh + 3, iy + 1), muted, uses[k].Who); }
					}
					if (over)
					{
						float mxp = ImGui::GetIO().MousePos.x;
						ImGui::BeginTooltip();
						ImGui::TextUnformatted(name.c_str());
						for (const Use& u : uses)
						{
							if (std::abs(x(double(u.Ms)) - mxp) > lh * 2) { continue; }
							ImGui::Text("%s  %s (%s)", Secs(u.Ms - peak).c_str(), u.Who.c_str(), u.Note.c_str());
						}
						ImGui::EndTooltip();
					}
				}
			}
		}

		// The side's skills in the 6 s around the peak, most damage first (the user, 2026-10-01: the enemy spike's list
		// is a quick view of what did the most; ours should have it too). A click shows a skill on the graph above.
		void TopSkills(const Ctx& c, const Spike& sp, bool aEnemy)
		{
			const Fight& f = *c.F;
			State& s = S();
			std::vector<int32_t>& picked = aEnemy ? s.PickedEnemy : s.Picked;
			float lh = ImGui::GetTextLineHeight();
			const int64_t peak = aEnemy ? sp.T : sp.Peak;
			const int64_t from = peak - kSpan, to = peak + kSpan;
			struct Row { double Damage = 0; int Hits = 0; std::map<std::string, double> By; };
			std::map<int32_t, Row> by;
			double total = 0;
			for (const Player& p : f.Players)
			{
				for (const auto& h : aEnemy ? p.HitsIn : p.HitsOut)
				{
					if (h.Ms < from || h.Ms >= to || h.Skill <= 0) { continue; }
					Row& r = by[h.Skill];
					r.Damage += h.Damage;
					r.Hits++;
					r.By[aEnemy ? (h.Enemy >= 0 ? f.Enemies[h.Enemy].Spec : std::string("NPC or siege")) : p.Name] += h.Damage;
					total += h.Damage;
				}
			}
			std::vector<std::pair<int32_t, const Row*>> skills;
			for (auto& [sk, r] : by) { skills.push_back({sk, &r}); }
			std::sort(skills.begin(), skills.end(), [](auto& a, auto& b) { return a.second->Damage > b.second->Damage; });
			ImGui::Spacing();
			ImGui::TextUnformatted(aEnemy ? "What hit us" : "What hit them");
			ImGui::SameLine(0, 12);
			ImGui::TextColored(kMuted, "%s in the 6 s around the peak; click a skill to show it on the graph above", Num(total).c_str());
			if (skills.empty()) { ImGui::TextColored(kMuted, "No hits in these 6 s."); return; }
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
			if (ImGui::BeginTable(aEnemy ? "whathitus" : "whathitthem", 5, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthFixed, 230);
				ImGui::TableSetupColumn(aEnemy ? "Enemy" : "Player", ImGuiTableColumnFlags_WidthFixed, 130);
				ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 40);
				ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 64);
				ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthStretch);
				Headers({{"Skill", nullptr}, {aEnemy ? "Enemy" : "Player", "Who did most of it"}, {"Hits", nullptr}, {"Damage", aEnemy ? "On any of us" : "To enemy players"},
					{"Share", aEnemy ? "Of their damage here" : "Of our damage here"}});
				double rest = 0;
				for (size_t i = 0; i < skills.size(); i++)
				{
					auto& [sk, r] = skills[i];
					if (i >= 8) { rest += r->Damage; continue; }
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					std::string name = SkillName(f, sk);
					bool marked = std::find(picked.begin(), picked.end(), sk) != picked.end();
					ImGui::PushID(sk);
					ImVec2 p = ImGui::GetCursorScreenPos();
					if (ImGui::Selectable("##sk", marked, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, lh)))
					{
						if (marked) { picked.erase(std::find(picked.begin(), picked.end(), sk)); }
						else { picked.push_back(sk); }
					}
					if (ImGui::IsItemHovered()) { ImGui::SetTooltip(marked ? "On the graph above: click to remove" : "Click: show it on the graph above"); }
					ImGui::PopID();
					IconAt(dl, p, lh, sk, name, aEnemy ? kEnemy : 0);
					dl->AddText(ImVec2(p.x + lh + 6, p.y), ink, name.c_str());
					std::string who;
					double most = 0;
					for (auto& [w, d] : r->By) { if (d > most) { most = d; who = w; } }
					Cell(who, &kMuted);
					NumCell(std::to_string(r->Hits));
					NumCell(Num(r->Damage));
					ImGui::TableNextColumn();
					ImVec2 bp = ImGui::GetCursorScreenPos();
					float bw = static_cast<float>(r->Damage / total) * ImGui::GetContentRegionAvail().x;
					Rect(dl, ImVec2(bp.x, bp.y + lh * 0.3f), std::max(2.0f, bw), lh * 0.4f, aEnemy ? kEnemy : kYou);
					ImGui::Dummy(ImVec2(1, lh));
				}
				ImGui::EndTable();
				if (rest > 0) { ImGui::TextColored(kMuted, "and %s from %d more skills", Num(rest).c_str(), static_cast<int>(skills.size()) - 8); }
			}
		}

		// ---- one of our spikes, broken down -------------------------------------------------------------------------

		void Breakdown(const Ctx& c, const View& v, const Spike& sp)
		{
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

			// the graph and who used the picked skills, then our skills here, most damage first
			SpikeGraph(c, sp, false);
			TopSkills(c, sp, false);
			constexpr int kBins = 2 * kSpan / kBin;
			const float labelW = 200;
			float width = ImGui::GetContentRegionAvail().x;
			ImDrawList* dl = ImGui::GetWindowDrawList();

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
				dl->AddLine(ImVec2(cxPeak, lp.y), ImVec2(cxPeak, lp.y + rh), ImGui::GetColorU32(ImGuiCol_Text), 1.5f);
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
					// cut to the room left (it was clipped at the window's edge); the whole reason on hover
					float wx = p.x + dmgX + 22 + ImGui::CalcTextSize(timing.c_str()).x;
					float room = p.x + width - wx - 4;
					std::string why = l.Why;
					auto small = [](const std::string& t) { return ImGui::CalcTextSize(t.c_str()).x * 0.85f; };
					if (small(why) > room)
					{
						while (!why.empty() && small(why + "...") > room) { why.pop_back(); }
						why += "...";
					}
					SmallText(dl, ImVec2(wx, p.y + 3), muted, why);
					if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s: %s", l.P->Name.c_str(), l.Why.c_str()); }
				}
			}
			ImGui::TextColored(kMuted, "On time: damage centred within 1 s of the peak.");
		}

		// An enemy spike broken down (the user's v7 pick: proposal A with B's list of what hit us): their damage to us
		// around the peak, the enemy skills that did it, most first, and who they hit, most damage first. The downs are
		// in Deaths ("Allied downs >").
		void EnemyBreakdown(const Ctx& c, const View& v, const Spike& sp)
		{
			const Fight& f = *c.F;
			State& s = S();
			float lh = ImGui::GetTextLineHeight();
			std::vector<const Spike*> theirs;
			for (const Spike& x : v.Spikes) { if (!x.Ours) { theirs.push_back(&x); } }
			size_t at = std::find(theirs.begin(), theirs.end(), &sp) - theirs.begin();
			if (ImGui::Button("< All spikes")) { s.SpikeOpen = -1; return; }
			ImGui::SameLine(0, 12);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Enemy spike at %s", Duration(sp.T).c_str());
			ImGui::SameLine(0, 10);
			ImGui::TextColored(kMuted, "%s", sp.Line.c_str());
			float stepW = ImGui::CalcTextSize("< Spike").x + ImGui::CalcTextSize("Spike >").x + ImGui::CalcTextSize("00 of 00").x + ImGui::GetStyle().FramePadding.x * 4 + 24;
			ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowContentRegionMax().x - stepW));
			if (ImGui::SmallButton("< Spike") && at > 0) { s.SpikeOpen = theirs[at - 1]->T; }
			ImGui::SameLine();
			ImGui::TextColored(kMuted, "%d of %d", static_cast<int>(at + 1), static_cast<int>(theirs.size()));
			ImGui::SameLine();
			if (ImGui::SmallButton("Spike >") && at + 1 < theirs.size()) { s.SpikeOpen = theirs[at + 1]->T; }

			const int64_t from = sp.T - kSpan, to = sp.T + kSpan;
			constexpr int kBins = 2 * kSpan / kBin;
			// their damage, per skill and per ally hit
			struct Hit { const Player* P; double Damage = 0; std::array<double, kBins> Bins{}; };
			std::map<const Player*, Hit> byAlly;
			std::map<int32_t, std::pair<double, int>> bySkill;
			std::map<int, double> bySubgroup;
			std::set<int> enemies;
			double total = 0;
			for (const Player& p : f.Players)
			{
				for (const auto& h : p.HitsIn)
				{
					if (h.Ms < from || h.Ms >= to) { continue; }
					size_t b = static_cast<size_t>((h.Ms - from) / kBin);
					auto& a = byAlly[&p];
					a.P = &p; a.Damage += h.Damage; a.Bins[b] += h.Damage;
					bySkill[h.Skill].first += h.Damage; bySkill[h.Skill].second++;
					if (h.Enemy >= 0) { enemies.insert(h.Enemy); }
					bySubgroup[p.Subgroup] += h.Damage;
					total += h.Damage;
				}
			}
			std::vector<std::pair<int32_t, std::pair<double, int>>> skills(bySkill.begin(), bySkill.end());
			std::sort(skills.begin(), skills.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
			std::vector<Hit> allies;
			for (auto& [p, a] : byAlly) { allies.push_back(a); }
			std::sort(allies.begin(), allies.end(), [](const Hit& a, const Hit& b) { return a.Damage > b.Damage; });

			// the answer
			if (total > 0)
			{
				int topGroup = -1;
				for (auto& [g, d] : bySubgroup) { if (topGroup < 0 || d > bySubgroup[topGroup]) { topGroup = g; } }
				std::string top2;
				double share = 0;
				for (size_t i = 0; i < skills.size() && i < 2; i++) { top2 += (i ? " and " : "") + SkillName(f, skills[i].first); share += skills[i].second.first; }
				Answer(std::to_string(enemies.size()) + " enemies hit " + std::to_string(allies.size()) + " of ours: " + top2 + " did " +
					std::to_string(int(100 * share / total + 0.5)) + "% of it, and subgroup " + std::to_string(topGroup) + " took " +
					std::to_string(int(100 * bySubgroup[topGroup] / total + 0.5)) + "%.");
			}
			else { Answer("No enemy damage on us around this spike."); return; }

			// their damage and the picked enemy skills on the spike's own clock (as in ours), then what hit us, most first
			SpikeGraph(c, sp, true);
			TopSkills(c, sp, true);
			const float labelW = 200;
			float width = ImGui::GetContentRegionAvail().x;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(kMuted);

			// who they hit: their damage on each of ours around the peak, most first
			ImGui::Spacing();
			ImGui::TextUnformatted("Who they hit");
			ImGui::SameLine(0, 12);
			ImGui::TextColored(kMuted, "damage taken around the peak, most first");
			double laneMx = 1;
			for (const Hit& a : allies) { for (double d : a.Bins) { laneMx = std::max(laneMx, d); } }
			const float laneX = labelW, laneW = std::max(200.0f, width - labelW - 330), dmgX = laneX + laneW + 70;
			size_t shown = std::min<size_t>(allies.size(), 10);
			for (size_t i = 0; i < shown; i++)
			{
				const Hit& a = allies[i];
				ImVec2 p = ImGui::GetCursorScreenPos();
				float rh = lh + 4;
				ImGui::Dummy(ImVec2(width, rh));
				SpecIconAt(dl, ImVec2(p.x, p.y + 2), lh, *a.P);
				std::string who = a.P->Name + (a.P->Pov ? " (you)" : "") + "  sg " + std::to_string(a.P->Subgroup);
				dl->PushClipRect(p, ImVec2(p.x + laneX - 6, p.y + rh), true);
				dl->AddText(ImVec2(p.x + lh + 5, p.y + 2), ink, who.c_str());
				dl->PopClipRect();
				ImVec2 lp(p.x + laneX, p.y);
				dl->AddRectFilled(lp, ImVec2(lp.x + laneW, lp.y + rh), kLaneBg);
				float bwid = laneW / kBins;
				for (int b = 0; b < kBins; b++)
				{
					float hgt = static_cast<float>(a.Bins[b] / laneMx) * (rh - 2);
					if (hgt > 0) { dl->AddRectFilled(ImVec2(lp.x + b * bwid + 1, lp.y + rh - hgt), ImVec2(lp.x + (b + 1) * bwid - 1, lp.y + rh), kEnemy); }
				}
				float cx = lp.x + laneW * 0.5f;
				dl->AddLine(ImVec2(cx, lp.y), ImVec2(cx, lp.y + rh), ink, 1.5f);
				std::string dmg = Num(a.Damage);
				dl->AddText(ImVec2(p.x + dmgX - ImGui::CalcTextSize(dmg.c_str()).x, p.y + 2), ink, dmg.c_str());
				// what stood out for them in the 3 s before the peak: stability taken, CC
				std::string why;
				for (const auto& st : a.P->StripsIn) { if (st.Boon == Analysis::kStability && st.Ms >= sp.T - 3000 && st.Ms <= sp.T + 1000) { why = std::string("stability ") + (st.Corrupted ? "corrupted " : "stripped ") + Secs(st.Ms - sp.T); break; } }
				for (const auto& h : a.P->CcIn)
				{
					if (h.Ms < sp.T - 3000 || h.Ms > sp.T + 1000) { continue; }
					why += (why.empty() ? "" : "; ") + std::string(Analysis::kCcVerbs[h.Kind]) + " " + Secs(h.Ms - sp.T) + (HadBoonAt(*a.P, Analysis::kStability, h.Ms - 50) ? ", stability on" : ", no stability");
					break;
				}
				if (!why.empty())
				{
					// cut to the room left, the whole of it on hover
					std::string cut = why;
					float room = p.x + width - (p.x + dmgX + 12) - 4;
					auto small = [](const std::string& txt) { return ImGui::CalcTextSize(txt.c_str()).x * 0.85f; };
					if (small(cut) > room) { while (!cut.empty() && small(cut + "...") > room) { cut.pop_back(); } cut += "..."; }
					SmallText(dl, ImVec2(p.x + dmgX + 12, p.y + 3), muted, cut);
					if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s: %s", a.P->Name.c_str(), why.c_str()); }
				}
			}
			if (allies.size() > shown)
			{
				double rest = 0;
				for (size_t i = shown; i < allies.size(); i++) { rest += allies[i].Damage; }
				ImGui::TextColored(kMuted, "and %d more took %s", static_cast<int>(allies.size() - shown), Num(rest).c_str());
			}
			if (sp.Downs > 0)
			{
				ImGui::Spacing();
				if (ImGui::Button("Allied downs in this spike >")) { s.DeathSpike = sp.T; s.DeathKey.clear(); s.DeathFilter = 0; s.SwitchTo = T_Deaths; }
			}
		}
	}

	void RoundTab(const Ctx& c)
	{
		const Fight& f = *c.F;
		State& s = S();
		const View& v = ViewOf(c);
		if (s.SpikeOpen >= 0 && s.SpikeStamp == f.Stamp)
		{
			for (const Spike& sp : v.Spikes)
			{
				if (sp.T != s.SpikeOpen || sp.Ours == s.SpikeEnemy) { continue; }
				if (sp.Ours) { Breakdown(c, v, sp); } else { EnemyBreakdown(c, v, sp); }
				return;
			}
		}
		s.SpikeOpen = -1;

		// Stability over time opens from the Stability card ("Over time >"; the Spikes / Stability over time switch did the
		// same, the user, 2026-09-30) and has its way back
		if (s.RoundView == 1)
		{
			if (ImGui::Button("< Round")) { s.RoundView = 0; }
			ImGui::SameLine(0, 12);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Stability over time");
			StabilityTimeLine(c);
			return;
		}
		// The verdict
		std::string verdict = f.EnemyDowns > f.SquadDowns ? "Won" : f.EnemyDowns < f.SquadDowns ? "Lost" : "Even";
		verdict += ": we downed " + std::to_string(f.EnemyDowns) + (f.EnemyDowns == 1 ? " enemy" : " enemies") + ", they downed " + std::to_string(f.SquadDowns) +
			(f.SquadDowns == 1 ? " ally" : " allies") + " (" + std::to_string(f.SquadDeaths) + " died).";
		ImGui::SetWindowFontScale(1.15f);
		ImGui::TextUnformatted(verdict.c_str());
		ImGui::SetWindowFontScale(1.0f);
		Overview(c, v);
	}

	SpikeTally OurSpikeTally(const Ctx& c)
	{
		SpikeTally t;
		for (const Spike& s : Build(c).Spikes)
		{
			if (!s.Ours) { continue; }
			t.Spikes++;
			t.OnTime += s.OnTime;
			t.Alive += s.Alive;
		}
		return t;
	}
}
