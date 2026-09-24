#include "Ui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "Session.h"
#include "UiCommon.h"

namespace Ui
{
	bool ShowWindow = false;
	int ForcedTab = -1;
	int ForcedMetric = -1;
	int ForcedOpen = -1;

	namespace
	{
		// Mouse wheel over our window, when ImGui doesn't get it by itself (see OnMouseWheel)
		std::atomic<bool> s_Hovered{false};
		std::atomic<int>  s_PendingWheel{0};
		ImGuiWindow*      s_Window = nullptr;

		bool IsOurs(ImGuiWindow* aWindow)
		{
			for (ImGuiWindow* w = aWindow; w; w = w->ParentWindow) { if (w == s_Window) { return true; } }
			return false;
		}

		// Scrolls what's under the mouse with a wheel turn ImGui didn't see itself, the way ImGui would: the
		// hovered window, or the nearest parent that can scroll.
		void ApplyWheel()
		{
			int pending = s_PendingWheel.exchange(0);
			ImGuiContext& g = *ImGui::GetCurrentContext();
			if (pending == 0 || g.IO.MouseWheel != 0.0f || !s_Window) { return; }
			ImGuiWindow* w = g.HoveredWindow;
			if (!w || !IsOurs(w)) { return; }
			while ((w->Flags & ImGuiWindowFlags_ChildWindow) && w->ScrollMax.y == 0.0f && w->ParentWindow) { w = w->ParentWindow; }
			float step = ImFloor(ImMin(5 * w->CalcFontSize(), w->InnerRect.GetHeight() * 0.67f));
			ImGui::SetScrollY(w, w->Scroll.y - (pending / 120.0f) * step);
		}

		std::string RoundTitle(const Fight& x, int aIndex)
		{
			char buf[128];
			std::snprintf(buf, sizeof(buf), "Round %d  %s  %s  %d v %d  downed %d / %d%s", aIndex + 1, Clock(x.Stamp).c_str(),
				Duration(x.DurationMs).c_str(), x.SquadCount, x.EnemyCount, x.EnemyDowns, x.SquadDowns, x.DurationMs < 20000 ? "  (short)" : "");
			return buf;
		}

		// Round, This round / Tonight, and who you're compared with: shared by every tab
		void HeaderRow(const Session::Snapshot& aSnap, int aIndex, const Ctx& c)
		{
			State& s = S();
			int count = static_cast<int>(aSnap.Fights.size());
			ImGui::SetNextItemWidth(300);
			if (ImGui::BeginCombo("##round", RoundTitle(*aSnap.Fights[aIndex], aIndex).c_str()))
			{
				for (int i = count - 1; i >= 0; i--)
				{
					if (ImGui::Selectable(RoundTitle(*aSnap.Fights[i], i).c_str(), i == aIndex)) { s.Selected = i == count - 1 ? -1 : i; s.OpenFix = -1; }
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Downed: enemies we downed / ours downed"); }
			ImGui::SameLine();
			bool follow = s.Selected < 0;
			if (ImGui::Checkbox("Latest", &follow)) { s.Selected = follow ? -1 : aIndex; }
			if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Jump to each new round"); }
			ImGui::SameLine(0, 16);
			if (ImGui::RadioButton("This round", !s.AllRounds)) { s.AllRounds = false; }
			ImGui::SameLine();
			if (ImGui::RadioButton("Tonight", s.AllRounds)) { s.AllRounds = true; }
			if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Every round loaded, added up"); }
			if (!c.MeRaw) { return; }
			ImGui::SameLine(0, 16);
			ImGui::TextColored(kMuted, "vs");
			ImGui::SameLine();
			std::string current = c.Vs ? (s.VsAccount.empty() ? "best " + c.PeerLabel() + ": " : "") + c.Vs->Name : "nobody";
			ImGui::SetNextItemWidth(std::max(150.0f, ImGui::GetContentRegionAvail().x));
			if (ImGui::BeginCombo("##vs", current.c_str()))
			{
				const Metric& m = Metrics()[c.RoleMetric];
				if (ImGui::Selectable(("best " + c.PeerLabel()).c_str(), s.VsAccount.empty())) { s.VsAccount.clear(); }
				for (const Player& p : c.Peers)
				{
					std::string rate = Known(m, p) ? Num(Rate(m, p, m.Total(p))) + " " + RateUnit(m) : std::string("unknown");
					std::string item = p.Name + (c.SameSpec ? "" : " (" + p.Spec + ")") + "   " + m.Name + " " + rate + "##" + p.Account;
					if (ImGui::Selectable(item.c_str(), &p == c.Vs && !s.VsAccount.empty())) { s.VsAccount = p.Account; }
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Who you're compared with"); }
		}
	}

	bool OnMouseWheel(int aDelta)
	{
		if (!ShowWindow || !s_Hovered.load()) { return false; }
		s_PendingWheel.fetch_add(aDelta);
		return true;
	}

	void Render()
	{
		if (!ShowWindow) { s_Hovered = false; return; }
		ApplyWheel();
		ImGui::SetNextWindowSize(ImVec2(980, 760), ImGuiCond_FirstUseEver);
		bool open = ImGui::Begin(kWindowName, &ShowWindow);
		s_Window = ImGui::GetCurrentWindow();
		{
			ImGuiContext& g = *ImGui::GetCurrentContext();
			s_Hovered = g.HoveredWindow && IsOurs(g.HoveredWindow);
		}
		if (!open) { ImGui::End(); return; }

		Session::Snapshot snap = Session::Get();
		if (snap.Fights.empty())
		{
			ImGui::TextWrapped("No fights yet. Logs appear here a few seconds after ArcDPS saves them.");
			ImGui::TextColored(kMuted, "Watching %s", snap.Folder.string().c_str());
			if (!snap.Status.empty()) { ImGui::TextColored(kMuted, "%s", snap.Status.c_str()); }
			ImGui::End();
			return;
		}
		State& s = S();
		if (ForcedMetric >= 0) { s.Metric = ForcedMetric; }
		if (ForcedOpen >= 0) { s.OpenFix = ForcedOpen; }
		int count = static_cast<int>(snap.Fights.size());
		int index = s.Selected < 0 || s.Selected >= count ? count - 1 : s.Selected;
		Ctx c = BuildCtx(snap.Fights, index, s.AllRounds);

		HeaderRow(snap, index, c);
		if (!snap.Status.empty()) { ImGui::TextColored(kMuted, "%s", snap.Status.c_str()); }

		int want = ForcedTab >= 0 ? ForcedTab : s.SwitchTo;
		s.SwitchTo = -1;
		auto tab = [want](const char* aName, int aIndex)
		{
			return ImGui::BeginTabItem(aName, nullptr, want == aIndex ? ImGuiTabItemFlags_SetSelected : 0);
		};
		if (ImGui::BeginTabBar("tabs"))
		{
			if (tab("Review", T_Review)) { ReviewTab(c); ImGui::EndTabItem(); }
			if (tab("Compare", T_Compare)) { CompareTab(c); ImGui::EndTabItem(); }
			if (tab("Squad", T_Squad)) { SquadTab(c); ImGui::EndTabItem(); }
			if (tab("Fight", T_Fight)) { FightTab(c); ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}
		ImGui::End();
	}

	void Options()
	{
		Session::Snapshot snap = Session::Get();
		ImGui::Text("Log folder: %s", snap.Folder.string().c_str());
		ImGui::Text("Rounds loaded: %d", static_cast<int>(snap.Fights.size()));
		ImGui::Checkbox("Show the Fight Review window", &ShowWindow);
	}
}
